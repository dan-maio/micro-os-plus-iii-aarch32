// smp_test_int3 — third external-interrupt test (stage A)
//
// PC keyboard → board, fully interrupt-driven:
//   - USB OTG1 (0xFF780000) runs as a vendor gadget (2207:001B). A host
//     tool (host/kbd_forward) grabs the PC keyboard via evdev and forwards
//     every key press/release as an 8-byte record over bulk EP1-OUT.
//     Every bus reset / enumeration step / EP0 setup / packet completion
//     is a GIC interrupt serviced on CORE 0 — the first interrupt-driven
//     USB in this port (smp_test6/7 poll).
//   - UART1 stays interrupt-driven on CORE 1 (RX echo + THRE TX ring,
//     inherited from smp_test_int2) — the first test with two peripheral
//     IRQs live on two different cores simultaneously.
//   - The OTG1 GIC interrupt ID is NOT taken from documentation: it is
//     discovered at runtime by ISPENDR snapshot diffing (lesson D3:
//     Rockchip "SPI n" naming is untrustworthy; verify on the wire).
//
// USB code lives in usb/ (project policy: shared src/usb stays untouched).

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <gic.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <test-console.hpp>

extern "C" unsigned port_cpu_id(void);

// usb/usb1_int3.cpp
extern "C" {
  void usb1i3_init(void);
  void usb1i3_service(void);
  int  usb1i3_is_configured(void);
  void usb1i3_reg_snapshot(uint32_t out[12]);
  void usb1i3_ep1_snapshot(uint32_t out[8]);
  extern volatile uint32_t usb1i3_dbg[13];
}

static constexpr uint32_t UART1_IRQ_ID = gic::irq::UART1;   // GIC ID 67

// OTG1 GIC ID — discovered at runtime (0 = not found yet)
static volatile uint32_t g_usb1_irq_id = 0;

// DesignWare 8250 extras not in rk3506.hpp (local to this test)
namespace uart1x {
  using USR = rk3506::Register<std::uint32_t, rk3506::base::UART1 + 0x7C>;
  namespace ier {
    inline constexpr std::uint32_t ERBFI = 1u << 0;  // RX data available
    inline constexpr std::uint32_t ETBEI = 1u << 1;  // THR empty
  }
}

// ---------------------------------------------------------------------------
// Statistics (ISRs write, threads read)
// ---------------------------------------------------------------------------
static volatile uint32_t g_rx_count = 0;                    // UART chars
static volatile uint32_t g_uart_irq_core[3] = {0, 0, 0};
static volatile uint32_t g_usb_irq_core[3]  = {0, 0, 0};
static volatile uint32_t g_tx_irq_count = 0;
static volatile uint32_t g_txq_dropped = 0;
static volatile uint32_t g_key_events = 0;                  // records received
static volatile uint32_t g_key_presses = 0;                 // value==1 only
static volatile uint16_t g_last_key_code = 0;

// ---------------------------------------------------------------------------
// TX ring buffer + lock (verbatim from smp_test_int2 — whole lines enqueued
// under one cpsid-i + LDREX/STREX lock; THRE ISR drains; \n -> CR+LF)
// ---------------------------------------------------------------------------
static constexpr uint32_t TXQ_SIZE = 4096;                 // power of two
static char              g_txq[TXQ_SIZE];
static volatile uint32_t g_txq_head = 0;
static volatile uint32_t g_txq_tail = 0;
static volatile uint32_t g_txq_lock = 0;
static volatile uint32_t g_txq_active = 0;                 // 0 = polled TX (boot)

static uint32_t txq_lock_acquire()
{
  uint32_t cpsr;
  __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
  __asm__ volatile("cpsid i" ::: "memory");
  while (__atomic_exchange_n (&g_txq_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
    ;
  return cpsr;
}

static void txq_lock_release(uint32_t cpsr)
{
  __atomic_store_n (&g_txq_lock, 0u, __ATOMIC_RELEASE);
  if (!(cpsr & 0x80U))
    __asm__ volatile("cpsie i" ::: "memory");
}

static bool txq_putc_raw_locked(char c)
{
  uint32_t next = (g_txq_head + 1) & (TXQ_SIZE - 1);
  if (next == g_txq_tail)
    {
      g_txq_dropped = g_txq_dropped + 1;
      return false;
    }
  g_txq[g_txq_head] = c;
  g_txq_head = next;
  return true;
}

static bool txq_putc_locked(char c)
{
  if (c == '\n')
    txq_putc_raw_locked('\r');
  return txq_putc_raw_locked(c);
}

static void txq_kick_locked()
{
  rk3506::uart1::IER::set_bits(uart1x::ier::ETBEI);
}

static void txq_write(const char* s)
{
  if (!g_txq_active)
    {
      uart::uart1 << s;
      return;
    }
  uint32_t cpsr = txq_lock_acquire();
  while (*s)
    txq_putc_locked(*s++);
  txq_kick_locked();
  txq_lock_release(cpsr);
}

// Console hook for the USB driver (usb/usb1_int3.cpp)
extern "C" void app_console_write(const char* s) { txq_write(s); }

// ---------------------------------------------------------------------------
// Tiny formatting helpers
// ---------------------------------------------------------------------------
static char* append_str(char* p, const char* s)
{
  while (*s) *p++ = *s++;
  return p;
}

static char* append_dec(char* p, uint32_t v)
{
  char tmp[10];
  int n = 0;
  do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
  while (n) *p++ = tmp[--n];
  return p;
}

static char* append_hex(char* p, uint32_t v)
{
  for (int i = 28; i >= 0; i -= 4)
    *p++ = "0123456789ABCDEF"[(v >> i) & 0xF];
  return p;
}

// ---------------------------------------------------------------------------
// LED helpers (GPIO1_A0)
// ---------------------------------------------------------------------------
static void led_init()
{
  using namespace rk3506;
  cru::CLKGATE_CON22::clear_bits(0x0002);
  cru::CLKGATE_CON03::clear_bits(0x0300);
  cru::SOFTRST_CON22::clear_bits(0x0002);
  cru::SOFTRST_CON03::clear_bits(0x0300);
  gpio1_ioc::GPIO1A_IOMUX_SEL0::write(0x000F, 0x0000);
  gpio1::SWPORT_DDR_L::write(0x0001, 0x0001);
}

static void led_on()  { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0001); }
static void led_off() { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0000); }

// ---------------------------------------------------------------------------
// Linux input-event key names (common subset; others print numerically)
// ---------------------------------------------------------------------------
static const char* key_name(uint16_t code)
{
  static const char* names[] = {
    /*0*/ nullptr, "ESC", "1", "2", "3", "4", "5", "6", "7", "8",
    /*10*/ "9", "0", "-", "=", "BACKSPACE", "TAB", "q", "w", "e", "r",
    /*20*/ "t", "y", "u", "i", "o", "p", "[", "]", "ENTER", "LCTRL",
    /*30*/ "a", "s", "d", "f", "g", "h", "j", "k", "l", ";",
    /*40*/ "'", "`", "LSHIFT", "\\", "z", "x", "c", "v", "b", "n",
    /*50*/ "m", ",", ".", "/", "RSHIFT", "KP*", "LALT", "SPACE", "CAPSLOCK", "F1",
  };
  if (code < sizeof(names) / sizeof(names[0]) && names[code])
    return names[code];
  return nullptr;
}

// ---------------------------------------------------------------------------
// Keystroke records from the host forwarder — 8 bytes each:
//   [0]='K' [1]=value (0=up,1=down,2=repeat) [2..3]=code LE [4..7]=seq LE
// Called from usb1i3_service() — ISR context on whatever core the USB IRQ
// is routed to.
// ---------------------------------------------------------------------------
static volatile uint32_t g_led_pulse = 0;   // LED thread consumes

extern "C" void usb1i3_rx_packet(const uint8_t* pkt, uint32_t len)
{
  char line[64];
  for (uint32_t off = 0; off + 8 <= len; off += 8)
    {
      const uint8_t* r = pkt + off;
      if (r[0] != 'K')
        continue;
      uint8_t  value = r[1];
      uint16_t code  = static_cast<uint16_t>(r[2] | (r[3] << 8));
      g_key_events = g_key_events + 1;
      g_last_key_code = code;
      if (value == 1)
        {
          g_key_presses = g_key_presses + 1;
          g_led_pulse = g_led_pulse + 1;
        }

      char* p = line;
      p = append_str(p, "[KEY]  core=");
      p = append_dec(p, port_cpu_id());
      p = append_str(p, " ");
      const char* nm = key_name(code);
      if (nm)
        p = append_str(p, nm);
      else
        {
          p = append_str(p, "code=");
          p = append_dec(p, code);
        }
      p = append_str(p, value == 0 ? " up" : (value == 1 ? " DOWN" : " rep"));
      p = append_str(p, "\n");
      *p = 0;
      txq_write(line);
    }
}

// ---------------------------------------------------------------------------
// External IRQ handler — TWO peripheral sources on TWO different cores:
//   UART1 (GIC ID 67)      -> core 1 (ITARGETSR)
//   USB OTG1 (discovered)  -> core 0 (ITARGETSR)
// ---------------------------------------------------------------------------
extern "C" void port_external_irq_handler(uint32_t iar)
{
  using namespace rk3506;

  uint32_t irq_id = iar & 0x3FF;

  if (irq_id == UART1_IRQ_ID)
    {
      g_uart_irq_core[port_cpu_id()] = g_uart_irq_core[port_cpu_id()] + 1;

      for (;;)
        {
          uint32_t iir = uart1::FCR_IIR::read() & 0x0F;
          if (iir & 0x01)
            break;
          if ((iir & 0x06) == 0x04)             // RDA / char timeout
            {
              uint32_t cpsr = txq_lock_acquire();
              while (uart1::LSR::test_bits(uart1::lsr::DR))
                {
                  char c = static_cast<char>(uart1::RBR_THR::read());
                  g_rx_count = g_rx_count + 1;
                  txq_putc_locked(c);           // echo via TX ring
                }
              txq_kick_locked();
              txq_lock_release(cpsr);
            }
          else if (iir == 0x02)                 // THR empty — refill FIFO
            {
              g_tx_irq_count = g_tx_irq_count + 1;
              uint32_t cpsr = txq_lock_acquire();
              for (int n = 0; n < 32 && g_txq_tail != g_txq_head; ++n)
                {
                  uart1::RBR_THR::write(
                      static_cast<std::uint32_t>(g_txq[g_txq_tail]));
                  g_txq_tail = (g_txq_tail + 1) & (TXQ_SIZE - 1);
                }
              if (g_txq_tail == g_txq_head)
                uart1::IER::clear_bits(uart1x::ier::ETBEI);
              txq_lock_release(cpsr);
            }
          else if (iir == 0x07)                 // DW busy detect
            {
              (void)uart1x::USR::read();
            }
          else
            {
              (void)uart1::LSR::read();
              break;
            }
        }
      return;
    }

  if (g_usb1_irq_id != 0 && irq_id == g_usb1_irq_id)
    {
      g_usb_irq_core[port_cpu_id()] = g_usb_irq_core[port_cpu_id()] + 1;
      usb1i3_service();
      return;
    }
}

// ---------------------------------------------------------------------------
// OTG1 GIC interrupt ID discovery (lesson D3: never trust "SPI n" docs).
// With the interrupt still DISABLED at the distributor, a level assertion
// sets its GICD_ISPENDR bit. Snapshot the pending state before the USB
// core is brought up, then diff: the first new SPI is OTG1.
// ---------------------------------------------------------------------------
static constexpr uint32_t PEND_WORDS = 8;      // GIC IDs 0..255
static uint32_t g_pend_base[PEND_WORDS];

static void gic_pend_snapshot(uint32_t* dst)
{
  for (uint32_t i = 0; i < PEND_WORDS; ++i)
    dst[i] = *reinterpret_cast<volatile uint32_t*>(
        gic::gicd::ISPENDR_BASE + i * 4);
}

// Returns the lowest newly-pending SPI (>=32, not UART1), or 0.
static uint32_t gic_find_new_spi()
{
  for (uint32_t i = 1; i < PEND_WORDS; ++i)     // skip word 0 (SGI/PPI)
    {
      uint32_t cur = *reinterpret_cast<volatile uint32_t*>(
          gic::gicd::ISPENDR_BASE + i * 4);
      uint32_t fresh = cur & ~g_pend_base[i];
      while (fresh)
        {
          uint32_t bit = static_cast<uint32_t>(__builtin_ctz(fresh));
          uint32_t id = i * 32 + bit;
          if (id != UART1_IRQ_ID)
            return id;
          fresh &= fresh - 1;
        }
    }
  return 0;
}

// ---------------------------------------------------------------------------
// Stack declarations for threads
// ---------------------------------------------------------------------------
static os::rtos::thread::stack::element_t stats_stack[2048];
static os::rtos::thread::stack::element_t led_stack[2048];

// ---------------------------------------------------------------------------
// Hardware init hooks
// ---------------------------------------------------------------------------
extern "C"
{
  void os_startup_initialize_hardware_early(void) { }

  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware(void)
  {
    uart::uart1.init();
    uart::uart1 << "\n\n";
    uart::uart1 << "+=============================================+\n";
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST INT3           |\n";
    uart::uart1 << "|  PC keyboard -> board via USB OTG1 (IRQ)    |\n";
    uart::uart1 << "|  UART1 IRQ on core 1, USB IRQ on core 0     |\n";
    uart::uart1 << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start, __heap_end - __heap_start);
    exception::init();
    led_init();
  }

  [[noreturn]] static void custom_main_trampoline(void)
  {
    int code = os_main(0, nullptr);
    std::exit(code);
  }

  extern os::rtos::thread* os_main_thread;

  extern "C" int main(void)
  {
    using namespace os::rtos;
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack()->set(
        reinterpret_cast<os::rtos::thread::stack::element_t*>(__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack()->initialize();
#endif
    os::trace::printf("\nµOS++ IIIe version " OS_STRING_RTOS_IMPL_VERSION "\n");
    os::rtos::port::scheduler::greeting();
    scheduler::initialize();
    thread::attributes attr = thread::initializer;
    attr.th_stack_size_bytes = OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES;
    os_main_thread = new thread("main",
                                reinterpret_cast<thread::func_t>(custom_main_trampoline),
                                nullptr, attr);
    os_startup_create_thread_idle();
    scheduler::start();
    return 0;
  }
}

// ---------------------------------------------------------------------------
// Core 0 thread: statistics reporter
// ---------------------------------------------------------------------------
static void* stats_thread_func(void*)
{
  char line[200];
  for (;;)
    {
      os::rtos::sysclock.sleep_for(2000);
      char* p = line;
      p = append_str(p, "\n[STAT] core=");
      p = append_dec(p, port_cpu_id());
      p = append_str(p, "  keys=");
      p = append_dec(p, g_key_presses);
      p = append_str(p, "/");
      p = append_dec(p, g_key_events);
      p = append_str(p, "  usbIRQ c0=");
      p = append_dec(p, g_usb_irq_core[0]);
      p = append_str(p, " c1=");
      p = append_dec(p, g_usb_irq_core[1]);
      p = append_str(p, "  uartIRQ c0=");
      p = append_dec(p, g_uart_irq_core[0]);
      p = append_str(p, " c1=");
      p = append_dec(p, g_uart_irq_core[1]);
      p = append_str(p, "  RX=");
      p = append_dec(p, g_rx_count);
      p = append_str(p, "  cfg=");
      p = append_dec(p, static_cast<uint32_t>(usb1i3_is_configured()));
      p = append_str(p, "  drop=");
      p = append_dec(p, g_txq_dropped);
      p = append_str(p, "\n");
      *p = 0;
      txq_write(line);

#if defined(SMP_TEST_INT3_DIAG)
      // Once configured, dump EP1-OUT state (keystroke path diagnostics).
      if (usb1i3_is_configured())
        {
          char dline[200];
          uint32_t e[8];
          usb1i3_ep1_snapshot(e);
          p = dline;
          p = append_str(p, "[EP1] CTL=");
          p = append_hex(p, e[0]);
          p = append_str(p, " INT=");
          p = append_hex(p, e[1]);
          p = append_str(p, " TSIZ=");
          p = append_hex(p, e[2]);
          p = append_str(p, " DMA=");
          p = append_hex(p, e[3]);
          p = append_str(p, " DAINT=");
          p = append_hex(p, e[4]);
          p = append_str(p, " MSK=");
          p = append_hex(p, e[5]);
          p = append_str(p, " pend=");
          p = append_dec(p, e[6]);
          p = append_str(p, " GINTSTS=");
          p = append_hex(p, e[7]);
          p = append_str(p, " ep1done=");
          p = append_dec(p, usb1i3_dbg[6]);
          p = append_str(p, "\n");
          *p = 0;
          txq_write(dline);
        }

      // While enumeration hasn't completed, dump the USB debug state:
      // event counters + control-path registers (detector at the source).
      if (!usb1i3_is_configured() && g_usb1_irq_id != 0)
        {
          char dline[220];
          p = dline;
          p = append_str(p, "[USBDBG] svc=");
          p = append_dec(p, usb1i3_dbg[0]);
          p = append_str(p, " rst=");
          p = append_dec(p, usb1i3_dbg[1]);
          p = append_str(p, " enum=");
          p = append_dec(p, usb1i3_dbg[2]);
          p = append_str(p, " outep=");
          p = append_dec(p, usb1i3_dbg[3]);
          p = append_str(p, " setup=");
          p = append_dec(p, usb1i3_dbg[4]);
          p = append_str(p, " inep=");
          p = append_dec(p, usb1i3_dbg[5]);
          p = append_str(p, " lastSts=");
          p = append_hex(p, usb1i3_dbg[7]);
          p = append_str(p, "\n[USBDBG] stalls=");
          p = append_dec(p, usb1i3_dbg[10]);
          p = append_str(p, " ep0tx=");
          p = append_dec(p, usb1i3_dbg[11]);
          p = append_str(p, " setupW0=");
          p = append_hex(p, usb1i3_dbg[8]);
          p = append_str(p, " setupW1=");
          p = append_hex(p, usb1i3_dbg[9]);
          p = append_str(p, " dmaNow=");
          p = append_hex(p, usb1i3_dbg[12]);
          p = append_str(p, "\n");
          *p = 0;
          txq_write(dline);

          uint32_t rg[12];
          usb1i3_reg_snapshot(rg);
          static const char* nm[12] = {
            "GINTSTS", "GINTMSK", "DAINT", "DAINTMSK", "DOEPCTL0", "DOEPINT0",
            "DOEPTSIZ0", "DOEPDMA0", "DCFG", "DSTS", "DCTL", "GAHBCFG" };
          p = dline;
          p = append_str(p, "[USBREG]");
          for (int i = 0; i < 12; ++i)
            {
              p = append_str(p, " ");
              p = append_str(p, nm[i]);
              p = append_str(p, "=");
              p = append_hex(p, rg[i]);
              if (i == 5)
                {
                  p = append_str(p, "\n");
                  *p = 0;
                  txq_write(dline);
                  p = dline;
                  p = append_str(p, "[USBREG]");
                }
            }
          p = append_str(p, "\n");
          *p = 0;
          txq_write(dline);
        }
#endif // SMP_TEST_INT3_DIAG
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Core 1 thread: LED — heartbeat every 2 s, fast pulse on every key press
// ---------------------------------------------------------------------------
static void* led_thread_func(void*)
{
  uint32_t seen_pulse = 0;
  unsigned tick = 0;
  for (;;)
    {
      os::rtos::sysclock.sleep_for(50);
      tick++;
      if (g_led_pulse != seen_pulse)            // key press -> 100ms flash
        {
          seen_pulse = g_led_pulse;
          led_on();
          os::rtos::sysclock.sleep_for(100);
          led_off();
        }
      else if ((tick % 40) == 0)                // 2s heartbeat blink
        {
          led_on();
          os::rtos::sysclock.sleep_for(30);
          led_off();
        }
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// os_main — entry point for the main (boot) thread on Core 0
// ---------------------------------------------------------------------------
int os_main(int argc, char* argv[])
{
  using namespace os::rtos;
  using uart::uart1;

  // 1. Enable IRQs globally (timer only so far)
  __asm__ volatile("cpsie i" ::: "memory");
  uart1 << "IRQs enabled (timer only)\n\n";

  // 2. Release core 1 (core 2 stays parked in BootROM)
  smp_install_boot_threads();

  uart1 << "Setting up SRAM mailbox for core 1...\n";
  rk3506::mailbox::TARGET::write(0x00200000u);
  rk3506::mailbox::MAGIC::write(rk3506::mailbox::MAGIC_VALUE);
  rk3506::dsb();

  uart1 << "Releasing core 1 from reset...\n";
  rk3506::cru::SOFTRST_CON00::clear_bits(rk3506::cru::softrst00::CORE1_RESETS);
  rk3506::dsb();
  rk3506::isb();
  rk3506::sev();

  uart1 << "Waiting for core 1 to join...\n";
  int waited = 0;
  while ((g_core_stage[1] < 3) && waited < 3000)
    {
      sysclock.sleep_for(50);
      waited += 50;
    }
  if (g_core_stage[1] >= 3)
    uart1 << "Core 1 joined after " << waited << "ms\n\n";
  else
    uart1 << "TIMEOUT waiting for core 1 (stage=" << g_core_stage[1] << ")\n";

  // 3. UART1 interrupt -> CORE 1 (as proven in smp_test_int2)
  uart1 << "Routing UART1 IRQ (GIC ID " << UART1_IRQ_ID << ") to core 1...\n";
  gic::gic.set_target(UART1_IRQ_ID, 1u << 1);
  gic::gic.enable_irq(UART1_IRQ_ID);
  rk3506::uart1::IER::set_bits(uart1x::ier::ERBFI);

  // 4. Switch console TX to the interrupt-driven ring
  uart1 << "Switching TX to interrupt-driven ring buffer...\n";
  while (!rk3506::uart1::LSR::test_bits(rk3506::uart1::lsr::THRE))
    ;
  g_txq_active = 1;
  rk3506::dsb();
  txq_write("TX is now IRQ-driven (UART ISR on core 1)\n\n");

  // 5. Snapshot GIC pending state, THEN bring up OTG1. The first new SPI
  //    to pend after the core asserts its line is OTG1's GIC ID.
  txq_write("Snapshotting GICD_ISPENDR baseline...\n");
  gic_pend_snapshot(g_pend_base);

  txq_write("Bringing up USB OTG1 (device mode, 2207:001B)...\n");
  usb1i3_init();

  txq_write("Waiting for OTG1 GIC ID to reveal itself\n"
            "(plug the USB1 cable to the PC now if not connected)...\n");
  uint32_t found = 0;
  for (int ms = 0; ms < 30000 && !found; ms += 10)
    {
      found = gic_find_new_spi();
      if (!found)
        sysclock.sleep_for(10);
    }

  if (found)
    {
      char line[96];
      char* p = line;
      p = append_str(p, "OTG1 GIC interrupt ID discovered: ");
      p = append_dec(p, found);
      p = append_str(p, " (board-verified via ISPENDR)\n");
      *p = 0;
      txq_write(line);

      g_usb1_irq_id = found;
      rk3506::dsb();
      gic::gic.set_target(found, 1u << 0);      // USB ISR -> core 0
      gic::gic.enable_irq(found);
      txq_write("USB IRQ routed to core 0 and enabled\n\n");
    }
  else
    {
      txq_write("WARNING: no new pending SPI in 30s — USB stays silent.\n"
                "Check the cable, then reset. (Diagnostics: is 2207:001B\n"
                "visible in lsusb on the PC?)\n\n");
    }

  // 6. Application threads
  thread::attributes attr = thread::initializer;

  attr.th_stack_address = stats_stack;
  attr.th_stack_size_bytes = sizeof(stats_stack);
  static thread stats_thread{ "stats", stats_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT3_DYNAMIC_AFFINITY)
  // Dynamic variant: both threads migrate freely while the GIC keeps the
  // two peripheral IRQs pinned (UART1 -> core 1, USB OTG1 -> core 0).
  // Threads now contend for the TX-ring lock from cores that are each
  // servicing a different live external ISR, mid-migration — the
  // strongest publish/claim stress so far.
  stats_thread.cpu_affinity(0xFFFFFFFFu);
#else
  stats_thread.cpu_affinity(1u << 0);
#endif

  attr.th_stack_address = led_stack;
  attr.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread{ "led", led_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT3_DYNAMIC_AFFINITY)
  led_thread.cpu_affinity(0xFFFFFFFFu);
#else
  led_thread.cpu_affinity(1u << 1);
#endif

  txq_write("Threads created:\n"
#if defined(SMP_TEST_INT3_DYNAMIC_AFFINITY)
            "  [STAT] affinity=ANY core — stats every 2s\n"
            "  [LED]  affinity=ANY core — heartbeat 2s, pulse per key press\n"
            "  (dynamic scheduling: watch the core= fields migrate;\n"
            "   IRQs stay pinned by the GIC: UART1->core1, USB->core0)\n"
#else
            "  [STAT] core=0  — stats every 2s\n"
            "  [LED]  core=1  — heartbeat 2s, pulse per key press\n"
#endif
            "\nRun host/kbd_forward on the PC and type: every key press\n"
            "and release arrives as a USB interrupt on core 0.\n"
            "UART typing is still echoed by the core-1 ISR.\n"
            "---------------------------------------------------------------\n\n");

  // The verdict: the host must configure the OTG1 gadget within two minutes.
  // After that the test keeps running for one more minute, to type on the PC (host/kbd_forward),
  // then reports and exits.
  bool configured = false;
  for (unsigned s = 0; s < 120 && !configured; ++s)
    {
      sysclock.sleep_for(1000);
      configured = (usb1i3_is_configured() != 0);
    }
  uart1 << (configured ? "[USB] configured by the host; 60 s to type\n"
                       : "[USB] NOT configured by the host within 120 s\n");
  if (configured)
    sysclock.sleep_for(60000);

  test_result(configured);
}

// ---------------------------------------------------------------------------
// SMP idle threads for secondary cores
// ---------------------------------------------------------------------------
extern "C" {
extern os::rtos::thread* os_idle_thread;
}

namespace os { namespace rtos { namespace scheduler {
extern thread* os_idle_thread_core[3];
}}}

static os::rtos::thread::stack::element_t idle1_stack[1024];
static os::rtos::thread::stack::element_t idle2_stack[1024];
static os::rtos::thread* idle1_thread_ptr;
static os::rtos::thread* idle2_thread_ptr;

static void* secondary_idle_func(void*)
{
  os::rtos::this_thread::thread().priority(os::rtos::thread::priority::idle);
  while (true)
    {
      __asm__ volatile("dsb" ::: "memory");
      __asm__ volatile("wfi");
      os::rtos::this_thread::yield();
    }
  return nullptr;
}

extern "C" void smp_install_boot_threads(void)
{
  using namespace os::rtos;

  thread::attributes attr1 = thread::initializer;
  attr1.th_stack_address = idle1_stack;
  attr1.th_stack_size_bytes = sizeof(idle1_stack);
  idle1_thread_ptr = new thread("idle1", secondary_idle_func, nullptr, attr1);
  os::rtos::scheduler::os_idle_thread_core[1] = idle1_thread_ptr;

  thread::attributes attr2 = thread::initializer;
  attr2.th_stack_address = idle2_stack;
  attr2.th_stack_size_bytes = sizeof(idle2_stack);
  idle2_thread_ptr = new thread("idle2", secondary_idle_func, nullptr, attr2);
  os::rtos::scheduler::os_idle_thread_core[2] = idle2_thread_ptr;
}
