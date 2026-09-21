#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <gic.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>

extern "C" unsigned port_cpu_id(void);

// UART1 GIC interrupt ID = SPI 67 + 32 = 99
static constexpr uint32_t UART1_IRQ_ID = gic::irq::UART1;

// RX statistics (written by ISR, read by threads)
static volatile uint32_t g_uart_rx_count = 0;
static volatile char     g_last_rx_char  = 0;

// ---------------------------------------------------------------------------
// UART TX line lock — serializes whole lines between cores AND the RX-echo
// ISR. Holders run with local IRQs masked, so the echo ISR on core 0 can
// never interrupt a core-0 holder (no deadlock); a core-1 holder finishes
// its line while the ISR spins briefly.
// ---------------------------------------------------------------------------
static volatile uint32_t g_tx_lock = 0;

static uint32_t tx_lock_acquire()
{
  // Mask local IRQs first: a holder on this core can then never be
  // interrupted by the echo ISR (which also takes this lock).
  uint32_t cpsr;
  __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
  __asm__ volatile("cpsid i" ::: "memory");
  // Spin — the compiler emits the LDREX/STREX loop for the builtin.
  while (__atomic_exchange_n (&g_tx_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
    ;
  return cpsr;
}

static void tx_lock_release(uint32_t cpsr)
{
  __atomic_store_n (&g_tx_lock, 0u, __ATOMIC_RELEASE);
  if (!(cpsr & 0x80U))
    __asm__ volatile("cpsie i" ::: "memory");
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
// External IRQ handler — called from port_isr_handler for non-timer/non-IPI
// ---------------------------------------------------------------------------
extern "C" void port_external_irq_handler(uint32_t iar)
{
  uint32_t irq_id = iar & 0x3FF;
  if (irq_id != UART1_IRQ_ID)
    return;

  // Read IIR to determine interrupt source (same address as FCR, reading = IIR)
  uint32_t iir = rk3506::uart1::FCR_IIR::read();

  // IIR bit 0 = 0 → interrupt pending; bits 3:1 identify the source:
  //   0b010 (0x04) = RDA (Receive Data Available)
  //   0b110 (0x0C) = Character Timeout (FIFO mode)
  if ((iir & 0x01) != 0)
    return;                          // spurious

  // bits[3:1] must be 0b010 or 0b110 → (iir & 0x06) == 0x04 catches both
  if ((iir & 0x06) != 0x04)
    return;

  // Drain the RX FIFO and echo every byte back via polled TX.
  // The TX line lock keeps echoed characters from landing inside a
  // [LED]/[UART] line printed by a thread on either core.
  using namespace rk3506;
  uint32_t cpsr = tx_lock_acquire();
  while (uart1::LSR::test_bits(uart1::lsr::DR))
  {
    char c = static_cast<char>(uart1::RBR_THR::read());
    __atomic_add_fetch(&g_uart_rx_count, 1u, __ATOMIC_RELAXED);
    g_last_rx_char = c;

    // Echo — poll THRE then write
    while (!uart1::LSR::test_bits(uart1::lsr::THRE))
      ;
    uart1::RBR_THR::write(static_cast<std::uint32_t>(c));
  }
  tx_lock_release(cpsr);
}

// ---------------------------------------------------------------------------
// Stack declarations for threads
// ---------------------------------------------------------------------------
static os::rtos::thread::stack::element_t uart_stack[2048];
static os::rtos::thread::stack::element_t led_stack[2048];

// ---------------------------------------------------------------------------
// os_startup_initialize_hardware_early
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
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST INT            |\n";
    uart::uart1 << "|  UART1 interrupt-driven RX echo on Core 0   |\n";
    uart::uart1 << "|  LED blink on Core 1                        |\n";
    uart::uart1 << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start, __heap_end - __heap_start);
    exception::init();
    led_init();
  }

  // -----------------------------------------------------------------------
  // main trampoline
  // -----------------------------------------------------------------------
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
// Core 0 thread: UART status reporter
// ---------------------------------------------------------------------------
static void* uart_monitor_thread_func(void*)
{
  for (;;)
  {
    os::rtos::sysclock.sleep_for(2000);
    uint32_t cpsr = tx_lock_acquire();
    uart::uart1 << "\n[UART] core=" << static_cast<int>(port_cpu_id())
                << "  RX count=" << static_cast<int>(g_uart_rx_count)
                << "  last char=0x";
    uart::uart1.print_hex(static_cast<std::uint32_t>(g_last_rx_char), 2);
    uart::uart1 << " '" << (g_last_rx_char >= 0x20 ? g_last_rx_char : '.') << "'\n";
    tx_lock_release(cpsr);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Core 1 thread: LED blink (no UART usage — avoids TX contention)
// ---------------------------------------------------------------------------
static void* led_blink_thread_func(void*)
{
  unsigned count = 0;
  for (;;)
  {
    os::rtos::sysclock.sleep_for(1000);
    led_on();
    os::rtos::sysclock.sleep_for(100);
    led_off();
    count++;
    // Whole line under the TX lock - never interleaves with echo/stats
    uint32_t cpsr = tx_lock_acquire();
    uart::uart1 << "\n[LED]  core=" << static_cast<int>(port_cpu_id())
                << " blink #" << static_cast<int>(count) << "\n";
    tx_lock_release(cpsr);
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

  // 2. Release secondary cores (only core 1 — core 2 stays in BootROM)
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

  // 3. Enable UART1 interrupt *after* core 1 is stable
  uart1 << "Enabling UART1 interrupt (GIC ID " << UART1_IRQ_ID << ")...\n";
  gic::gic.enable_irq(UART1_IRQ_ID);
  rk3506::uart1::IER::set_bits(0x01);
  uart1 << "UART1 IER set to 0x";
  uart1.print_hex(rk3506::uart1::IER::read(), 2);
  uart1 << "\n";
  uart1 << "UART1 RX is now interrupt-driven\n\n";

  // 5. Create application threads with CPU affinity
  thread::attributes attr = thread::initializer;

  attr.th_stack_address = uart_stack;
  attr.th_stack_size_bytes = sizeof(uart_stack);
  static thread uart_thread{ "uart", uart_monitor_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT_DYNAMIC_AFFINITY)
  // Dynamic variant: both threads float across all cores. The UART1 IRQ
  // stays pinned to core 0 by the GIC (ITARGETSR), independent of where
  // the threads run — this stresses the scheduler's migration paths
  // (deferred publish/claim) against a live external interrupt.
  uart_thread.cpu_affinity(0xFFFFFFFFu);
#else
  uart_thread.cpu_affinity(1u << 0);         // pin to core 0
#endif

  attr.th_stack_address = led_stack;
  attr.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread{ "led", led_blink_thread_func, nullptr, attr };
#if defined(SMP_TEST_INT_DYNAMIC_AFFINITY)
  led_thread.cpu_affinity(0xFFFFFFFFu);
#else
  led_thread.cpu_affinity(1u << 1);          // pin to core 1
#endif

  uart1 << "Threads created:\n";
#if defined(SMP_TEST_INT_DYNAMIC_AFFINITY)
  uart1 << "  [UART] affinity=ANY core — prints RX stats every 2s\n";
  uart1 << "  [LED]  affinity=ANY core — blinks LED every 1s\n";
  uart1 << "  (dynamic scheduling: watch the core= field migrate;\n";
  uart1 << "   UART1 IRQ itself stays on core 0 via GIC ITARGETSR)\n";
#else
  uart1 << "  [UART] core=0  — prints RX stats every 2s\n";
  uart1 << "  [LED]  core=1  — blinks LED every 1s\n";
#endif
  uart1 << "\nType characters on the UART terminal — they are echoed via IRQ\n";
  uart1 << "---------------------------------------------------------------\n\n";

  // 6. Sleep forever (scheduler runs the two threads)
  for (;;)
    sysclock.sleep_for(10000);
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
