// smp_test_int4 — interrupt-driven keystroke logger (three cores, three IRQs)
//
// The milestone this test completes: every core services its own peripheral
// interrupt, and the data flows ISR -> thread -> ISR across cores, with no
// polling on any wait path.
//
//   PC keyboard --USB IRQ (core 0)--> keystroke ring --semaphore--> sdlog
//   UART console <--THRE-ring IRQ (core 1)-- stats/echo               |
//                                                     DW-MMC DTO IRQ (core 2)
//                                                                     v
//                                                        keys.log on the card
//
//   * USB OTG1  (GIC ID discovered)  -> core 0 : keystroke sink (from int3)
//   * UART1     (GIC ID 67)          -> core 1 : IRQ-driven RX echo + TX ring
//   * DW-MMC    (GIC ID discovered)  -> core 2 : data-transfer-over completion
//
// The USB ISR (core 0) enqueues each keystroke and posts g_key_avail; the
// sdlog thread (core 2) formats a line and appends it to keys.log via FatFS,
// whose block writes now complete on the DW-MMC DTO interrupt (core 2) that
// posts g_sd_done — the canonical ISR->thread wakeup, twice, across cores.
//
// Per project policy, per-test peripheral code lives in this folder:
//   sd/   — interrupt-completion copy of the DW-MMC driver (shared src/sdcard
//           untouched); the shared FatFS links against it automatically.
//   usb/  — reused from smp_test_int3 (referenced, not copied).

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <gic.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include "sdmmc.hpp"
#include "ff.hpp"

extern "C" unsigned port_cpu_id(void);

// usb/usb1_int3.cpp (referenced from smp_test_int3)
extern "C" {
  void usb1i3_init(void);
  void usb1i3_service(void);
  int  usb1i3_is_configured(void);
}

static constexpr uint32_t UART1_IRQ_ID = gic::irq::UART1;   // GIC ID 67

// Peripheral GIC IDs discovered at runtime (0 = not found yet)
static volatile uint32_t g_usb_irq_id = 0;
static volatile uint32_t g_sd_irq_id  = 0;

namespace uart1x {
  using USR = rk3506::Register<std::uint32_t, rk3506::base::UART1 + 0x7C>;
  namespace ier {
    inline constexpr std::uint32_t ERBFI = 1u << 0;  // RX data available
    inline constexpr std::uint32_t ETBEI = 1u << 1;  // THR empty
  }
}

// ---------------------------------------------------------------------------
// Statistics (ISRs + threads write, stats thread reads)
// ---------------------------------------------------------------------------
static volatile uint32_t g_rx_count = 0;                    // UART chars
static volatile uint32_t g_uart_irq_core[3] = {0, 0, 0};
static volatile uint32_t g_usb_irq_core[3]  = {0, 0, 0};
static volatile uint32_t g_sd_irq_core[3]   = {0, 0, 0};
static volatile uint32_t g_tx_irq_count = 0;
static volatile uint32_t g_txq_dropped  = 0;
static volatile uint32_t g_key_events   = 0;                // records received
static volatile uint32_t g_key_presses  = 0;                // value==1 only
static volatile uint32_t g_lines_logged = 0;                // lines written to SD
static volatile uint32_t g_sd_errors    = 0;
static volatile uint32_t g_led_pulse    = 0;

// ---------------------------------------------------------------------------
// TX ring buffer + lock (from smp_test_int2/3 — whole lines under one
// cpsid-i + LDREX/STREX lock; THRE ISR drains; \n -> CR+LF)
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
  if (next == g_txq_tail) { g_txq_dropped = g_txq_dropped + 1; return false; }
  g_txq[g_txq_head] = c;
  g_txq_head = next;
  return true;
}

static bool txq_putc_locked(char c)
{
  if (c == '\n') txq_putc_raw_locked('\r');
  return txq_putc_raw_locked(c);
}

static void txq_kick_locked() { rk3506::uart1::IER::set_bits(uart1x::ier::ETBEI); }

static void txq_write(const char* s)
{
  if (!g_txq_active) { uart::uart1 << s; return; }
  uint32_t cpsr = txq_lock_acquire();
  while (*s) txq_putc_locked(*s++);
  txq_kick_locked();
  txq_lock_release(cpsr);
}

extern "C" void app_console_write(const char* s) { txq_write(s); }

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static char* append_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* append_dec(char* p, uint32_t v)
{
  char tmp[10]; int n = 0;
  do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
  while (n) *p++ = tmp[--n];
  return p;
}
static char* append_dec_pad(char* p, uint32_t v, int width)
{
  char tmp[10]; int n = 0;
  do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
  for (int i = n; i < width; ++i) *p++ = '0';
  while (n) *p++ = tmp[--n];
  return p;
}

// ---------------------------------------------------------------------------
// LED (GPIO1_A0)
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
// Linux input-event key names (common subset)
// ---------------------------------------------------------------------------
static const char* key_name(uint16_t code)
{
  static const char* names[] = {
    nullptr, "ESC", "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "0", "-", "=", "BACKSPACE", "TAB", "q", "w", "e", "r",
    "t", "y", "u", "i", "o", "p", "[", "]", "ENTER", "LCTRL",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ";",
    "'", "`", "LSHIFT", "\\", "z", "x", "c", "v", "b", "n",
    "m", ",", ".", "/", "RSHIFT", "KP*", "LALT", "SPACE", "CAPSLOCK", "F1",
  };
  if (code < sizeof(names) / sizeof(names[0]) && names[code]) return names[code];
  return nullptr;
}

// ---------------------------------------------------------------------------
// Keystroke ring: USB ISR (core 0) produces, sdlog thread (core 2) consumes.
// Each slot is a fully-formatted 8-byte record from the host forwarder:
//   [0]='K' [1]=value (0=up,1=down,2=repeat) [2..3]=code LE [4..7]=seq LE
// A counting semaphore (g_key_avail) carries the wakeup across cores (IPI).
// ---------------------------------------------------------------------------
struct KeyRec { uint8_t value; uint16_t code; uint32_t seq; };
static constexpr uint32_t KQ_SIZE = 256;                   // power of two
static KeyRec               g_kq[KQ_SIZE];
static volatile uint32_t    g_kq_head = 0;                 // ISR writes
static volatile uint32_t    g_kq_tail = 0;                 // thread reads
static os::rtos::semaphore_counting g_key_avail{ KQ_SIZE, 0 };

extern "C" void usb1i3_rx_packet(const uint8_t* pkt, uint32_t len)
{
  for (uint32_t off = 0; off + 8 <= len; off += 8)
    {
      const uint8_t* r = pkt + off;
      if (r[0] != 'K') continue;
      uint32_t next = (g_kq_head + 1) & (KQ_SIZE - 1);
      if (next == g_kq_tail) continue;                     // ring full: drop
      g_kq[g_kq_head].value = r[1];
      g_kq[g_kq_head].code  = static_cast<uint16_t>(r[2] | (r[3] << 8));
      g_kq[g_kq_head].seq   = static_cast<uint32_t>(r[4] | (r[5] << 8)
                                        | (r[6] << 16) | (uint32_t(r[7]) << 24));
      g_kq_head = next;
      g_key_events = g_key_events + 1;
      if (r[1] == 1) { g_key_presses = g_key_presses + 1; g_led_pulse = g_led_pulse + 1; }
      g_key_avail.post();                                  // wake sdlog (cross-core)
    }
}

// ---------------------------------------------------------------------------
// SD interrupt-completion semaphore. The DW-MMC ISR (core 2) posts g_sd_done;
// the sdlog thread blocks inside the driver's write path on it. These override
// the weak hooks in sd/sdmmc.cpp.
// ---------------------------------------------------------------------------
static os::rtos::semaphore_binary g_sd_done{ 0 };

extern "C" void sd_completion_reset(void)
{
  // Drain any stale token so the next wait() blocks until a fresh DTO.
  while (g_sd_done.try_wait() == os::rtos::result::ok) { }
}
extern "C" void sd_completion_wait(void) { g_sd_done.wait(); }
extern "C" void sd_completion_post(void) { g_sd_done.post(); }

// ---------------------------------------------------------------------------
// External IRQ handler — THREE peripheral sources on THREE cores:
//   UART1 (GIC 67)  -> core 1     USB OTG1 (discovered) -> core 0
//   DW-MMC (discovered) -> core 2
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
          if (iir & 0x01) break;
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
                  uart1::RBR_THR::write(static_cast<std::uint32_t>(g_txq[g_txq_tail]));
                  g_txq_tail = (g_txq_tail + 1) & (TXQ_SIZE - 1);
                }
              if (g_txq_tail == g_txq_head) uart1::IER::clear_bits(uart1x::ier::ETBEI);
              txq_lock_release(cpsr);
            }
          else if (iir == 0x07) { (void)uart1x::USR::read(); }   // DW busy detect
          else { (void)uart1::LSR::read(); break; }
        }
      return;
    }

  if (g_usb_irq_id != 0 && irq_id == g_usb_irq_id)
    {
      g_usb_irq_core[port_cpu_id()] = g_usb_irq_core[port_cpu_id()] + 1;
      usb1i3_service();
      return;
    }

  if (g_sd_irq_id != 0 && irq_id == g_sd_irq_id)
    {
      g_sd_irq_core[port_cpu_id()] = g_sd_irq_core[port_cpu_id()] + 1;
      sdcard::sd_irq_service();
      return;
    }
}

// ---------------------------------------------------------------------------
// GIC pending-register discovery (lesson D5): a level IRQ sets its ISPENDR bit
// on assertion even while disabled at the distributor. Snapshot, trigger, diff.
// ---------------------------------------------------------------------------
static constexpr uint32_t PEND_WORDS = 8;      // GIC IDs 0..255
static uint32_t g_pend_base[PEND_WORDS];

static void gic_pend_snapshot(uint32_t* dst)
{
  for (uint32_t i = 0; i < PEND_WORDS; ++i)
    dst[i] = *reinterpret_cast<volatile uint32_t*>(gic::gicd::ISPENDR_BASE + i * 4);
}

// Lowest newly-pending SPI (>=32) not already claimed, or 0.
static uint32_t gic_find_new_spi(uint32_t exclude1, uint32_t exclude2)
{
  for (uint32_t i = 1; i < PEND_WORDS; ++i)
    {
      uint32_t cur = *reinterpret_cast<volatile uint32_t*>(gic::gicd::ISPENDR_BASE + i * 4);
      uint32_t fresh = cur & ~g_pend_base[i];
      while (fresh)
        {
          uint32_t bit = static_cast<uint32_t>(__builtin_ctz(fresh));
          uint32_t id = i * 32 + bit;
          if (id != UART1_IRQ_ID && id != exclude1 && id != exclude2) return id;
          fresh &= fresh - 1;
        }
    }
  return 0;
}

// ---------------------------------------------------------------------------
// FatFS log file
// ---------------------------------------------------------------------------
using namespace fatfs;
static FATFS g_fs;
static FIL   g_logfile;
static bool  g_log_open = false;

// ---------------------------------------------------------------------------
// Thread stacks
// ---------------------------------------------------------------------------
static os::rtos::thread::stack::element_t stats_stack[2048];
static os::rtos::thread::stack::element_t led_stack[2048];
static os::rtos::thread::stack::element_t sdlog_stack[4096];   // FatFS is stack-hungry

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
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST INT4           |\n";
    uart::uart1 << "|  Interrupt keystroke logger — 3 cores/3 IRQ |\n";
    uart::uart1 << "|  USB->c0  UART->c1  SD(DTO)->c2             |\n";
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
// Core 2 thread: sdlog — consumes keystrokes, appends to keys.log via FatFS.
// The f_write -> disk_write -> SdMmc::write_blocks path now completes on the
// DW-MMC DTO interrupt (posted to g_sd_done by sd_irq_service on core 2).
// ---------------------------------------------------------------------------
static void* sdlog_thread_func(void*)
{
  char line[80];
  unsigned since_sync = 0;
  for (;;)
    {
      g_key_avail.wait();                       // cross-core wakeup from USB ISR
      uint32_t tail = g_kq_tail;
      if (tail == g_kq_head) continue;          // spurious
      KeyRec rec = g_kq[tail];
      g_kq_tail = (tail + 1) & (KQ_SIZE - 1);

      if (!g_log_open) continue;                // no card: still drains the ring

      char* p = line;
      p = append_dec_pad(p, static_cast<uint32_t>(os::rtos::sysclock.now()), 8);
      p = append_str(p, " ms  core=");
      p = append_dec(p, port_cpu_id());
      p = append_str(p, "  KEY ");
      const char* nm = key_name(rec.code);
      if (nm) p = append_str(p, nm);
      else { p = append_str(p, "code="); p = append_dec(p, rec.code); }
      p = append_str(p, rec.value == 0 ? " up" : (rec.value == 1 ? " DOWN" : " rep"));
      p = append_str(p, "  seq="); p = append_dec(p, rec.seq);
      p = append_str(p, "\r\n");
      *p = 0;

      UINT bw = 0;
      FRESULT fr = f_write(&g_logfile, line, static_cast<UINT>(p - line), &bw);
      if (fr != FR_OK) { g_sd_errors = g_sd_errors + 1; continue; }
      g_lines_logged = g_lines_logged + 1;

      if (++since_sync >= 8)                     // periodic durability flush
        {
          since_sync = 0;
          if (f_sync(&g_logfile) != FR_OK) g_sd_errors = g_sd_errors + 1;
        }
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Core 0 thread: statistics reporter over the IRQ-driven UART ring
// ---------------------------------------------------------------------------
static void* stats_thread_func(void*)
{
  char line[220];
  for (;;)
    {
      os::rtos::sysclock.sleep_for(2000);
      char* p = line;
      p = append_str(p, "\n[STAT] core=");        p = append_dec(p, port_cpu_id());
      p = append_str(p, "  keys=");               p = append_dec(p, g_key_presses);
      p = append_str(p, "/");                     p = append_dec(p, g_key_events);
      p = append_str(p, "  logged=");             p = append_dec(p, g_lines_logged);
      p = append_str(p, "  sdErr=");              p = append_dec(p, g_sd_errors);
      p = append_str(p, "\n[STAT] irq  usb c0="); p = append_dec(p, g_usb_irq_core[0]);
      p = append_str(p, "  uart c1=");            p = append_dec(p, g_uart_irq_core[1]);
      p = append_str(p, "  sd c2=");              p = append_dec(p, g_sd_irq_core[2]);
      p = append_str(p, "  (c0="); p = append_dec(p, g_sd_irq_core[0]);
      p = append_str(p, " c1=");   p = append_dec(p, g_sd_irq_core[1]);
      p = append_str(p, ")\n");
      *p = 0;
      txq_write(line);
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Core 1 thread: LED — heartbeat every 2 s, fast pulse per key press
// ---------------------------------------------------------------------------
static void* led_thread_func(void*)
{
  uint32_t seen_pulse = 0;
  unsigned tick = 0;
  for (;;)
    {
      os::rtos::sysclock.sleep_for(50);
      tick++;
      if (g_led_pulse != seen_pulse)
        { seen_pulse = g_led_pulse; led_on(); os::rtos::sysclock.sleep_for(80); led_off(); }
      else if ((tick % 40) == 0)
        { led_on(); os::rtos::sysclock.sleep_for(30); led_off(); }
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// SD bring-up + FatFS mount (polled — runs before IRQ mode is armed)
// ---------------------------------------------------------------------------
extern sdcard::SdMmc& sd_disk();     // sd/diskio_sd_irq.cpp — the FatFS instance

static bool sd_bring_up()
{
  txq_write("Bringing up SD card (polled)...\n");
  if (!sd_disk().init())
    { txq_write("SD init FAILED\n"); return false; }
  sd_disk().set_transfer_clock(24000000u);       // 24 MHz transfer clock

  FRESULT fr = f_mount(&g_fs, "0:", 1);
  if (fr != FR_OK)
    { txq_write("f_mount FAILED\n"); return false; }

  fr = f_open(&g_logfile, "0:/keys.log", FA_OPEN_APPEND | FA_WRITE);
  if (fr != FR_OK)
    { txq_write("f_open keys.log FAILED\n"); return false; }

  const char* banner = "\r\n==== smp_test_int4 session start ====\r\n";
  UINT bw = 0;
  f_write(&g_logfile, banner, static_cast<UINT>(__builtin_strlen(banner)), &bw);
  f_sync(&g_logfile);
  g_log_open = true;
  txq_write("SD mounted, keys.log open (polled writes so far)\n");
  return true;
}

// ---------------------------------------------------------------------------
// os_main — boot thread on Core 0
// ---------------------------------------------------------------------------
int os_main(int argc, char* argv[])
{
  using namespace os::rtos;
  using uart::uart1;

  __asm__ volatile("cpsie i" ::: "memory");
  uart1 << "IRQs enabled (timer only)\n\n";

  // 1. Release cores 1 AND 2 (first INT test to use all three)
  smp_install_boot_threads();
  smp::start_secondary_cores();
  uart1 << "Waiting for cores 1 and 2 to join...\n";
  int waited = 0;
  while ((g_core_stage[1] < 3 || g_core_stage[2] < 3) && waited < 3000)
    { sysclock.sleep_for(50); waited += 50; }
  uart1 << "cores: c1=" << g_core_stage[1] << " c2=" << g_core_stage[2]
        << " (after " << waited << "ms)\n\n";

  // 2. UART1 IRQ -> core 1, then switch console TX to the IRQ ring
  uart1 << "Routing UART1 IRQ (GIC ID " << UART1_IRQ_ID << ") to core 1...\n";
  gic::gic.set_target(UART1_IRQ_ID, 1u << 1);
  gic::gic.enable_irq(UART1_IRQ_ID);
  rk3506::uart1::IER::set_bits(uart1x::ier::ERBFI);
  while (!rk3506::uart1::LSR::test_bits(rk3506::uart1::lsr::THRE)) ;
  g_txq_active = 1;
  rk3506::dsb();
  txq_write("TX is now IRQ-driven (UART ISR on core 1)\n\n");

  // 3. SD card: bring up + mount while still polled
  bool sd_ok = sd_bring_up();

  // 4. Discover + route the DW-MMC interrupt to core 2, then arm IRQ writes.
  //    Trigger an assertion by issuing one polled write (its DTO sets ISPENDR).
  if (sd_ok)
    {
      txq_write("Discovering DW-MMC GIC ID (ISPENDR diff)...\n");
      sd_disk().enable_hw_interrupts();            // unmask DTO at the controller
      gic_pend_snapshot(g_pend_base);
      // One more sync forces a data write whose DTO asserts the (masked-at-GIC) line.
      f_sync(&g_logfile);
      uint32_t found = 0;
      for (int ms = 0; ms < 2000 && !found; ms += 10)
        { found = gic_find_new_spi(g_usb_irq_id, 0); if (!found) sysclock.sleep_for(10); }
      if (found)
        {
          char l[80]; char* p = l;
          p = append_str(p, "DW-MMC GIC ID discovered: "); p = append_dec(p, found);
          p = append_str(p, " -> routing to core 2\n"); *p = 0;
          txq_write(l);
          g_sd_irq_id = found;
          rk3506::dsb();
          gic::gic.set_target(found, 1u << 2);  // SD ISR -> core 2
          gic::gic.enable_irq(found);
          sd_disk().set_irq_mode(true);            // writes now complete by IRQ
          txq_write("SD writes are now interrupt-completed on core 2\n\n");
        }
      else
        {
          txq_write("WARNING: DW-MMC GIC ID not found; SD stays polled\n\n");
        }
    }

  // 5. USB OTG1: discover + route to core 0
  txq_write("Bringing up USB OTG1 (2207:001B)...\n");
  gic_pend_snapshot(g_pend_base);
  usb1i3_init();
  txq_write("Waiting for USB GIC ID (plug the USB1 cable to the PC)...\n");
  uint32_t ufound = 0;
  for (int ms = 0; ms < 30000 && !ufound; ms += 10)
    { ufound = gic_find_new_spi(g_sd_irq_id, 0); if (!ufound) sysclock.sleep_for(10); }
  if (ufound)
    {
      char l[80]; char* p = l;
      p = append_str(p, "USB GIC ID discovered: "); p = append_dec(p, ufound);
      p = append_str(p, " -> routing to core 0\n"); *p = 0;
      txq_write(l);
      g_usb_irq_id = ufound;
      rk3506::dsb();
      gic::gic.set_target(ufound, 1u << 0);
      gic::gic.enable_irq(ufound);
      txq_write("USB IRQ routed to core 0 and enabled\n\n");
    }
  else
    txq_write("WARNING: USB GIC ID not found; keyboard input unavailable\n\n");

  // 6. Application threads: one per core, each also servicing an IRQ
  thread::attributes attr = thread::initializer;

  attr.th_stack_address = stats_stack; attr.th_stack_size_bytes = sizeof(stats_stack);
  static thread stats_thread{ "stats", stats_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT4_DYNAMIC_AFFINITY)
  stats_thread.cpu_affinity(0xFFFFFFFFu);
#else
  stats_thread.cpu_affinity(1u << 0);
#endif

  attr.th_stack_address = led_stack; attr.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread{ "led", led_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT4_DYNAMIC_AFFINITY)
  led_thread.cpu_affinity(0xFFFFFFFFu);
#else
  led_thread.cpu_affinity(1u << 1);
#endif

  attr.th_stack_address = sdlog_stack; attr.th_stack_size_bytes = sizeof(sdlog_stack);
  static thread sdlog_thread{ "sdlog", sdlog_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT4_DYNAMIC_AFFINITY)
  sdlog_thread.cpu_affinity(0xFFFFFFFFu);
#else
  sdlog_thread.cpu_affinity(1u << 2);
#endif

  txq_write("Threads created:\n"
#if defined(SMP_TEST_INT4_DYNAMIC_AFFINITY)
            "  [STAT] [LED] [sdlog] affinity=ANY — threads migrate;\n"
            "  IRQs stay pinned: USB->c0, UART->c1, SD->c2\n"
#else
            "  [STAT]  core 0   [LED] core 1   [sdlog] core 2\n"
#endif
            "\nType on the PC (run host/kbd_forward): each key is logged to\n"
            "keys.log on the SD card via the interrupt-completed write path.\n"
            "Pull the card afterward and read keys.log.\n"
            "---------------------------------------------------------------\n\n");

  for (;;)
    sysclock.sleep_for(10000);
}

// ---------------------------------------------------------------------------
// SMP idle threads for secondary cores
// ---------------------------------------------------------------------------
extern "C" { extern os::rtos::thread* os_idle_thread; }
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
  thread::attributes a1 = thread::initializer;
  a1.th_stack_address = idle1_stack; a1.th_stack_size_bytes = sizeof(idle1_stack);
  idle1_thread_ptr = new thread("idle1", secondary_idle_func, nullptr, a1);
  os::rtos::scheduler::os_idle_thread_core[1] = idle1_thread_ptr;

  thread::attributes a2 = thread::initializer;
  a2.th_stack_address = idle2_stack; a2.th_stack_size_bytes = sizeof(idle2_stack);
  idle2_thread_ptr = new thread("idle2", secondary_idle_func, nullptr, a2);
  os::rtos::scheduler::os_idle_thread_core[2] = idle2_thread_ptr;
}
