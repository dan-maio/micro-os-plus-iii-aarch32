/*
 * clock-test (Raspberry Pi Zero 2 W, SMP) -- the kernel's clocks against the
 * BCM2837 System Timer (1 MHz).
 *
 * 5000 ticks of sysclock, and hrclock over them, keep the System Timer's
 * time; hrclock.sleep_until (now () + 100 ms) lasts 100 ms (+1 tick) on core
 * 0 and on core 1, and the thread wakes promptly after core 0's tick on both
 * (now () - steady_now () small). It guards the port faults fixed together:
 * the tick re-armed one period from now in its interrupt (the clocks lost
 * the handler's latency every tick); hrclock.now() counting from power-on
 * while its sleeps count from the tick's start; the frequency recalibrated
 * by every core; and (kernel) the cross-core IPI of thread::resume() never
 * sent, so a thread made ready for core 1 waited for core 1's own tick.
 */
#include <cmsis-plus/rtos/os.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <hw_result.hpp>
#include <test-console.hpp>
#include <test-smp-boot.hpp>
#include <smp.hpp>

using namespace os::rtos;

extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void os_startup_initialize_hardware (void)
  {
    uart::uart1.init();
    uart::uart1 << "\n+== " PORT_BANNER_SHORT " µOS++ CLOCK TEST ==+\n";
    os_startup_initialize_free_store(__heap_start,
                                     static_cast<std::size_t>(__heap_end - __heap_start));
    exception::init();
  }

  [[noreturn]] static void custom_main_trampoline (void)
  {
    int code = os_main (0, nullptr);
    std::exit (code);
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    interrupts::stack ()->set (reinterpret_cast<thread::stack::element_t*>(__fiq_stack_top),
                               __irq_stack_top - __fiq_stack_top);
    interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();
    static thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof(main_stack);
#if defined(OS_USE_SMP_SCHEDULER)
    attr.th_cpu_affinity = 1u << 0;
#endif
    static thread main_thread { "main", reinterpret_cast<thread::func_t> (custom_main_trampoline), nullptr, attr };
    os_main_thread = &main_thread;
    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}

// The BCM2837 System Timer's counter (CLO), 1 MHz.
static std::uint32_t now_us (void) { return *reinterpret_cast<volatile std::uint32_t*> (0x3F003004u); }
static unsigned g_bad = 0;
static void ok (bool c, const char* what) { console ("  [%s] %s\n", c ? "ok" : "BAD", what); if (!c) ++g_bad; }

struct probe { std::uint32_t sleep_us; clock::timestamp_t gap; };
static probe g_p1;
static void measure (probe& p)
{
  const std::uint64_t f = hrclock.input_clock_frequency_hz ();
  hrclock.sleep_for (1);
  const std::uint32_t u = now_us ();
  hrclock.sleep_until (hrclock.now () + f / 10);
  p.sleep_us = now_us () - u;
  p.gap = hrclock.now () - hrclock.steady_now ();
}
static void* on_core1 (void*) { measure (g_p1); return nullptr; }

int os_main (int, char*[])
{
  (void)interrupts::uncritical_section::enter ();
  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  test_wait_secondaries (3000);
#if defined(HW_BUILD)
  const bool hw = true;
#else
  const bool hw = false; // QEMU: its cores are not real-time
#endif
  const std::uint32_t tol = hw ? 200 : 20000;
  console ("cntfreq %lu Hz, cycles_per_tick %lu\n", (unsigned long)hrclock.input_clock_frequency_hz (),
           (unsigned long)port::clock_highres::cycles_per_tick ());

  sysclock.sleep_for (1);
  const clock::timestamp_t s0 = sysclock.now (), h0 = hrclock.now ();
  const std::uint32_t u0 = now_us ();
  sysclock.sleep_for (5000);
  const std::uint32_t st = now_us () - u0;
  const std::uint32_t hr = (std::uint32_t)((hrclock.now () - h0) * 1000000u / hrclock.input_clock_frequency_hz ());
  console ("5000 ticks: sysclock +%lu, System Timer %lu us, hrclock %lu us\n", (unsigned long)(sysclock.now () - s0),
           (unsigned long)st, (unsigned long)hr);
  ok (st + tol >= 5000000 && st <= 5000000 + tol, "the tick keeps the System Timer's time");
  ok (hr + tol >= st && st + tol >= hr, "hrclock keeps the System Timer's time");

  probe p0;
  measure (p0);
  thread::attributes a = thread::initializer;
  a.th_stack_size_bytes = 8 * 1024;
  a.th_cpu_affinity = 1u << 1;
  thread* t = new thread ("core1", on_core1, nullptr, a);
  t->join ();
  delete t;
  for (unsigned c = 0; c < 2; ++c)
    {
      const probe& p = c == 0 ? p0 : g_p1;
      console ("core %u: hrclock.sleep_until(now + 100 ms) %lu us, now - steady_now %lu cycles\n", c,
               (unsigned long)p.sleep_us, (unsigned long)p.gap);
      ok (p.sleep_us + tol >= 100000 && p.sleep_us <= 101000 + tol, c == 0 ? "hrclock sleep 100 ms (core 0)" : "hrclock sleep 100 ms (core 1)");
      // Within a tenth of a tick on the board, two ticks under QEMU.
      const std::uint64_t cpt = port::clock_highres::cycles_per_tick ();
      ok (p.gap <= (hw ? cpt / 10 : 2 * cpt), c == 0 ? "woken promptly after the tick (core 0)" : "woken promptly after core 0's tick (core 1)");
    }
  if (g_bad == 0)
    { console ("RESULT: PASS (clock-test: tick, hrclock and cross-core wake)\n"); hw_result::ok (); }
  console ("RESULT: FAIL (%u failed checks)\n", g_bad);
  hw_result::fail ();
}
