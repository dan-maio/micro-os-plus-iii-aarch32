#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <test-console.hpp>

#include <cstring>

int usb_gadget_init(void);

extern "C" {
  void uart_puts(const char* s);
  void uart_putc(char c);
  char* hex32(uint32_t val);
  uint32_t uart_sink_avail(void);
  uint32_t uart_sink_read(void* dst, uint32_t max);
  int uart_sink_finished(void);
  void uart_sink_end(void);
}

namespace rk3506::usb::dwc2 {
  class Driver {
  public:
    static Driver& instance(void);
    void initialize(void);
    void handle_interrupt(void);
  };
}

static os::rtos::mutex uart_mtx("uart_lock");

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

extern "C" unsigned port_cpu_id(void);

extern "C"
{
  void os_startup_initialize_hardware_early (void) { }

  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware (void)
  {
    uart::uart1.init();
    uart::uart1 << "\n\n";
    uart::uart1 << "+=============================================+\n";
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST 6              |\n";
    uart::uart1 << "|  USB-to-UART Logger on 3 Cores              |\n";
    uart::uart1 << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start, __heap_end - __heap_start);
    exception::init();
    led_init();
  }

  [[noreturn]] static void custom_main_trampoline (void)
  {
    int code = os_main (0, nullptr);
    std::exit (code);
  }

  extern os::rtos::thread* os_main_thread;

  extern "C" int main (void)
  {
    using namespace os::rtos;
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*>(__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    os::trace::printf ("\nµOS++ IIIe version " OS_STRING_RTOS_IMPL_VERSION "\n");
    os::rtos::port::scheduler::greeting ();
    scheduler::initialize ();
    thread::attributes attr = thread::initializer;
    attr.th_stack_size_bytes = OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES;
    os_main_thread = new thread ("main", reinterpret_cast<thread::func_t> (custom_main_trampoline), nullptr, attr);
    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}

extern "C" volatile uint32_t g_core_stage[3];
extern "C" void smp_install_boot_threads(void);

static os::rtos::thread::stack::element_t usb_stack[16384];
static os::rtos::thread::stack::element_t log_stack[8192];
static os::rtos::thread::stack::element_t led_stack[1024];

static void* usb_thread_func(void*)
{
  unsigned core = port_cpu_id();
  uart_mtx.lock();
  uart::uart1 << "[USB] USB task started on core " << (int)core << "\n";
  uart_mtx.unlock();

  auto& dwc2 = rk3506::usb::dwc2::Driver::instance();
  dwc2.initialize();
  usb_gadget_init();

  uart_mtx.lock();
  uart::uart1 << "[USB] DWC2 initialized, waiting for host connection\n";
  uart_mtx.unlock();

  for (;;)
  {
    dwc2.handle_interrupt();

    extern bool device_connected;
    extern bool endpoints_enabled;
    extern bool read_pending;
    extern void usb_vendor_start_read_queue(void);

    if (device_connected && endpoints_enabled && !read_pending)
      usb_vendor_start_read_queue();

    os::rtos::this_thread::yield();
  }
  return nullptr;
}

static void* log_thread_func(void*)
{
  unsigned core = port_cpu_id();
  unsigned count = 0;

  uart_mtx.lock();
  uart::uart1 << "[LOG] Logger task started on core " << (int)core << "\n";
  uart_mtx.unlock();

  for (;;)
  {
    os::rtos::sysclock.sleep_for(1000);
    count++;
    uart_mtx.lock();
    uart::uart1 << "[LOG] heartbeat " << (int)count
                << "s on core " << (int)port_cpu_id() << "\n";
    uart_mtx.unlock();
  }
  return nullptr;
}

static void* led_thread_func(void*)
{
  unsigned core = port_cpu_id();
  unsigned count = 0;

  uart_mtx.lock();
  uart::uart1 << "[LED] LED blink task started on core " << (int)core << "\n";
  uart_mtx.unlock();

  for (;;)
  {
    led_on();
    os::rtos::sysclock.sleep_for(250);
    led_off();
    os::rtos::sysclock.sleep_for(250);
    count++;

    if ((count % 10) == 0)
    {
      uart_mtx.lock();
      uart::uart1 << "[LED] " << (int)(count / 2) << "s alive on core "
                  << (int)port_cpu_id() << "\n";
      uart_mtx.unlock();
    }
  }
  return nullptr;
}

int os_main (int argc, char* argv[])
{
  using namespace os::rtos;
  using uart::uart1;

  __asm__ volatile("cpsie i" ::: "memory");

  smp_install_boot_threads();
  smp::start_secondary_cores();

  int waited = 0;
  while ((g_core_stage[1] < 3 || g_core_stage[2] < 3) && waited < 3000)
  {
    sysclock.sleep_for(50);
    waited += 50;
  }
  if (g_core_stage[1] == 3 && g_core_stage[2] == 3)
    uart1 << "Both cores joined after " << waited << "ms\n";
  else
    uart1 << "TIMEOUT: c1=" << g_core_stage[1] << " c2=" << g_core_stage[2] << "\n";

  uart1 << "Starting USB/UART logger demo\n";
  uart1 << "Run: usb_test_host --big <file>  (echo protocol, data logged to UART)\n";
  uart1 << "Or:  usb_test_host --write <text>\n\n";

  thread::attributes attr_usb = thread::initializer;
  attr_usb.th_stack_address = usb_stack;
  attr_usb.th_stack_size_bytes = sizeof(usb_stack);
  static thread usb_thread("usb", usb_thread_func, nullptr, attr_usb);
  usb_thread.cpu_affinity(0xFFFFFFFF);

  thread::attributes attr_log = thread::initializer;
  attr_log.th_stack_address = log_stack;
  attr_log.th_stack_size_bytes = sizeof(log_stack);
  static thread log_thread("logger", log_thread_func, nullptr, attr_log);
  log_thread.cpu_affinity(0xFFFFFFFF);

  thread::attributes attr_led = thread::initializer;
  attr_led.th_stack_address = led_stack;
  attr_led.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread("led", led_thread_func, nullptr, attr_led);
  led_thread.cpu_affinity(0xFFFFFFFF);

  uart1 << "3 threads created: usb(any core), logger(any core), led(any core)\n";
  uart1 << "Dynamic scheduling: work distributed by SMP scheduler\n";

  // The verdict: the host must configure the gadget (its bulk endpoints get
  // enabled) within two minutes. After that the test keeps running for one
  // more minute, for usb_test_host --write/--big, then reports and exits.
  extern bool endpoints_enabled;
  bool configured = false;
  for (unsigned s = 0; s < 120 && !configured; ++s)
    {
      sysclock.sleep_for(1000);
      configured = endpoints_enabled;
    }
  uart_mtx.lock();
  uart1 << (configured ? "[USB] configured by the host; 60 s for usb_test_host\n"
                       : "[USB] NOT configured by the host within 120 s\n");
  uart_mtx.unlock();
  if (configured)
    sysclock.sleep_for(60000);

  test_result(configured);
}

extern "C" {
extern os::rtos::thread* os_idle_thread;
}

namespace os { namespace rtos { namespace scheduler {
extern thread* os_idle_thread_core[3];
}}}

static os::rtos::thread::stack::element_t idle1_stack[512];
static os::rtos::thread::stack::element_t idle2_stack[512];
static os::rtos::thread* idle1_thread_ptr;
static os::rtos::thread* idle2_thread_ptr;

static void* secondary_idle_func (void*)
{
  os::rtos::this_thread::thread ().priority (os::rtos::thread::priority::idle);
  while (true)
  {
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("wfi");
    os::rtos::this_thread::yield ();
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
