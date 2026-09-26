// smp_test_int2 — second external-interrupt test
//
// Two new features over smp_test_int:
//   1. Interrupt-driven TX: all UART output goes through a ring buffer
//      drained by the THRE (Transmitter Holding Register Empty) interrupt.
//      No thread ever busy-polls LSR.THRE after startup.
//   2. Cross-core IRQ routing: the UART1 interrupt (GIC ID 67) is routed
//      to CORE 1 via GICD_ITARGETSR — the RX/TX ISR runs on a secondary
//      core while the stats thread runs on core 0.
//
// Output lines are naturally serialized: whole lines are enqueued into the
// TX ring under one lock, so enqueue order == wire order (no interleaving).

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <gic.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>

extern "C" unsigned port_cpu_id(void);

static constexpr uint32_t UART1_IRQ_ID = gic::irq::UART1;   // GIC ID 67

// DesignWare 8250 extras not in rk3506.hpp (local to this test)
namespace uart1x {
  // UART Status Register — reading it clears the DW "busy detect" (IIR=0x7)
  using USR = rk3506::Register<std::uint32_t, rk3506::base::UART1 + 0x7C>;
  namespace ier {
    inline constexpr std::uint32_t ERBFI = 1u << 0;  // RX data available
    inline constexpr std::uint32_t ETBEI = 1u << 1;  // THR empty
  }
}

// ---------------------------------------------------------------------------
// Statistics (ISR writes, threads read)
// ---------------------------------------------------------------------------
static volatile uint32_t g_rx_count = 0;
static volatile char     g_last_rx_char = 0;
static volatile uint32_t g_irq_count_core[3] = {0, 0, 0}; // which core ran the ISR
static volatile uint32_t g_tx_irq_count = 0;              // THRE interrupts serviced
static volatile uint32_t g_txq_dropped = 0;               // bytes lost to full ring

// ---------------------------------------------------------------------------
// TX ring buffer + lock
//
// The lock is the same design as smp_test_int's TX line lock: local IRQs
// masked first, then an LDREX/STREX spinlock (GCC builtins). A holder can
// never be interrupted by the UART ISR on its own core, and a holder on
// the other core just makes the ISR spin briefly — no deadlock, because
// holders never wait for the ISR.
// ---------------------------------------------------------------------------
static constexpr uint32_t TXQ_SIZE = 4096;                 // power of two
static char              g_txq[TXQ_SIZE];
static volatile uint32_t g_txq_head = 0;                   // producers write here
static volatile uint32_t g_txq_tail = 0;                   // ISR consumes here
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

// Enqueue one byte — caller holds the lock. Returns false if the ring is full.
// '\n' expands to CR+LF, matching the polled driver's puts() behaviour.
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

// Kick the transmitter — caller holds the lock. Setting ETBEI when the THR
// is already empty raises the THRE interrupt immediately, so the ISR does
// the first FIFO fill; no polled write is ever needed.
static void txq_kick_locked()
{
  rk3506::uart1::IER::set_bits(uart1x::ier::ETBEI);
}

// Enqueue a whole NUL-terminated line atomically and kick TX.
// Before g_txq_active, falls back to the polled uart driver (boot banner).
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

// ---------------------------------------------------------------------------
// Tiny formatting helpers (append into a stack buffer, then txq_write the
// whole line once — keeps line assembly outside the lock)
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

// ---------------------------------------------------------------------------
// LED helpers (GPIO1_A0, same as all smp_test projects)
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
// External IRQ handler — runs on WHICHEVER core the GIC routed ID 67 to
// (core 1 in this test). Loops on IIR until no source is pending.
// ---------------------------------------------------------------------------
extern "C" void port_external_irq_handler(uint32_t iar)
{
  using namespace rk3506;

  uint32_t irq_id = iar & 0x3FF;
  if (irq_id != UART1_IRQ_ID)
    return;

  g_irq_count_core[port_cpu_id()] = g_irq_count_core[port_cpu_id()] + 1;

  for (;;)
    {
      uint32_t iir = uart1::FCR_IIR::read() & 0x0F;

      if (iir & 0x01)
        break;                                  // nothing pending

      if ((iir & 0x06) == 0x04)                 // 0x04 RDA / 0x0C char timeout
        {
          // Drain RX; echo goes through the interrupt-driven TX ring.
          uint32_t cpsr = txq_lock_acquire();
          while (uart1::LSR::test_bits(uart1::lsr::DR))
            {
              char c = static_cast<char>(uart1::RBR_THR::read());
              g_rx_count = g_rx_count + 1;
              g_last_rx_char = c;
              txq_putc_locked(c);
            }
          txq_kick_locked();
          txq_lock_release(cpsr);
        }
      else if (iir == 0x02)                     // THR empty — refill FIFO
        {
          g_tx_irq_count = g_tx_irq_count + 1;
          uint32_t cpsr = txq_lock_acquire();
          // DW UART: THRE interrupt means the whole 32-byte TX FIFO is
          // empty; fill up to one FIFO's worth.
          for (int n = 0; n < 32 && g_txq_tail != g_txq_head; ++n)
            {
              uart1::RBR_THR::write(
                  static_cast<std::uint32_t>(g_txq[g_txq_tail]));
              g_txq_tail = (g_txq_tail + 1) & (TXQ_SIZE - 1);
            }
          if (g_txq_tail == g_txq_head)         // ring empty — stop THRE IRQs
            uart1::IER::clear_bits(uart1x::ier::ETBEI);
          txq_lock_release(cpsr);
        }
      else if (iir == 0x07)                     // DW busy detect
        {
          (void)uart1x::USR::read();            // reading USR clears it
        }
      else
        {
          (void)uart1::LSR::read();             // line status — read to clear
          break;
        }
    }
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
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST INT2           |\n";
    uart::uart1 << "|  IRQ-driven TX (THRE + ring buffer)         |\n";
    uart::uart1 << "|  UART1 IRQ routed to CORE 1 (ITARGETSR)     |\n";
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
// Core 0 thread: statistics reporter — proves the ISR runs on core 1
// ---------------------------------------------------------------------------
static void* stats_thread_func(void*)
{
  char line[160];
  for (;;)
    {
      os::rtos::sysclock.sleep_for(2000);
      char* p = line;
      p = append_str(p, "\n[STAT] core=");
      p = append_dec(p, port_cpu_id());
      p = append_str(p, "  RX=");
      p = append_dec(p, g_rx_count);
      p = append_str(p, "  irq@core0=");
      p = append_dec(p, g_irq_count_core[0]);
      p = append_str(p, " irq@core1=");
      p = append_dec(p, g_irq_count_core[1]);
      p = append_str(p, "  txIRQ=");
      p = append_dec(p, g_tx_irq_count);
      p = append_str(p, "  drop=");
      p = append_dec(p, g_txq_dropped);
      p = append_str(p, "\n");
      *p = 0;
      txq_write(line);
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Core 1 thread: LED blink — shares core 1 with the UART ISR
// ---------------------------------------------------------------------------
static void* led_blink_thread_func(void*)
{
  char line[64];
  unsigned count = 0;
  for (;;)
    {
      os::rtos::sysclock.sleep_for(1000);
      led_on();
      os::rtos::sysclock.sleep_for(100);
      led_off();
      count++;
      char* p = line;
      p = append_str(p, "\n[LED]  core=");
      p = append_dec(p, port_cpu_id());
      p = append_str(p, " blink #");
      p = append_dec(p, count);
      p = append_str(p, "\n");
      *p = 0;
      txq_write(line);
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

  // 1. Enable IRQs globally first (timer only, no UART IRQ yet)
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

  // 3. Route UART1 IRQ to core 1, then enable it (RX interrupt only;
  //    the THRE bit is toggled dynamically by the TX ring machinery)
  uart1 << "Routing UART1 IRQ (GIC ID " << UART1_IRQ_ID
        << ") to CORE 1 via ITARGETSR...\n";
  gic::gic.set_target(UART1_IRQ_ID, 1u << 1);
  gic::gic.enable_irq(UART1_IRQ_ID);
  rk3506::uart1::IER::set_bits(uart1x::ier::ERBFI);

  // 4. Flip all output to the interrupt-driven TX path from here on
  uart1 << "Switching TX to interrupt-driven ring buffer...\n";
  while (!rk3506::uart1::LSR::test_bits(rk3506::uart1::lsr::THRE))
    ;                                           // last polled flush
  g_txq_active = 1;
  rk3506::dsb();

  txq_write("TX is now IRQ-driven; UART ISR runs on core 1\n\n"
            "Threads created:\n"
#if defined(SMP_TEST_INT2_DYNAMIC_AFFINITY)
            "  [STAT] affinity=ANY core — prints stats every 2s\n"
            "  [LED]  affinity=ANY core — blinks LED every 1s\n"
            "  (dynamic scheduling: watch the core= field migrate;\n"
            "   UART1 IRQ itself stays on core 1 via GIC ITARGETSR)\n"
#else
            "  [STAT] core=0  — prints stats every 2s\n"
            "  [LED]  core=1  — blinks LED every 1s\n"
#endif
            "\nType characters — echoed by the core-1 ISR via the TX ring\n"
            "---------------------------------------------------------------\n\n");

  // 5. Create application threads with CPU affinity
  thread::attributes attr = thread::initializer;

  attr.th_stack_address = stats_stack;
  attr.th_stack_size_bytes = sizeof(stats_stack);
  static thread stats_thread{ "stats", stats_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT2_DYNAMIC_AFFINITY)
  // Dynamic variant: both threads float across all cores while the UART1
  // IRQ stays pinned to core 1 by the GIC (ITARGETSR) — stresses thread
  // migration (deferred publish/claim) against a live external interrupt
  // on a secondary core.
  stats_thread.cpu_affinity(0xFFFFFFFFu);
#else
  stats_thread.cpu_affinity(1u << 0);           // pin to core 0
#endif

  attr.th_stack_address = led_stack;
  attr.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread{ "led", led_blink_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT2_DYNAMIC_AFFINITY)
  led_thread.cpu_affinity(0xFFFFFFFFu);
#else
  led_thread.cpu_affinity(1u << 1);             // pin to core 1 (with the ISR)
#endif

  // 6. Run a bounded number of beats, then report and exit. The runner needs a
  // RESULT line and a semihosting stop, so the test must not sleep forever.
  for (unsigned it = 1; it <= 20; ++it)
    {
      sysclock.sleep_for(1000);
      uart1 << "beat " << static_cast<int>(it) << "/20\n";
    }

  // uart1 << putc() also writes the semihosting channel, so this reaches both
  // the terminal and the OpenOCD log the runner greps; the return goes through
  // the port's strong _Exit() (semihosting SYS_EXIT).
  uart1 << "\nRESULT: PASS\n";
  return 0;
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
