#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <test-console.hpp>

// The card through µOS++ POSIX I/O: its FAT volume mounted at /sd/ (the
// posix-io block device /dev/sd with Chan FatFs, uos_test_storage_start()).
#include <cmsis-plus/posix-io/directory.h>
#include <cmsis-plus/posix-io/file-system.h>
#include <cmsis-plus/posix-io/io.h>
#include <cmsis-plus/posix/sys/statvfs.h>
#include <uos-test-storage.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <cstdio>

static os::rtos::mutex uart_mtx("uart_lock");

// === UART RX (polled) ===
static char uart_getc() {
    using namespace rk3506;
    while (!uart1::LSR::test_bits(0x01)) { }
    return (char)uart1::RBR_THR::read();
}

// === External file buffer (shared with USB vendor gadget) ===
alignas(64) static uint8_t file_buffer[512 * 1024] __attribute__((section(".dma_nc")));
uint8_t* g_file_buf = file_buffer;
uint32_t g_file_max = sizeof(file_buffer);
uint32_t g_file_len = 0;
volatile int g_file_ready = 0;
volatile int g_led_blink = 0;  // set by SD task when delete confirmed

// === LED ===
static void led_init() {
    using namespace rk3506;
    cru::CLKGATE_CON22::clear_bits(0x0002); cru::CLKGATE_CON03::clear_bits(0x0300);
    cru::SOFTRST_CON22::clear_bits(0x0002); cru::SOFTRST_CON03::clear_bits(0x0300);
    gpio1_ioc::GPIO1A_IOMUX_SEL0::write(0x000F, 0x0000);
    gpio1::SWPORT_DDR_L::write(0x0001, 0x0001);
}
static void led_on()  { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0001); }
static void led_off() { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0000); }

extern "C" unsigned port_cpu_id(void);

extern "C"
{
  void os_startup_initialize_hardware_early (void) { }
  extern char __heap_start[], __heap_end[], __fiq_stack_top[], __irq_stack_top[];

  void os_startup_initialize_hardware (void) {
    uart::uart1.init();
    uart::uart1 << "\n\n+=============================================+\n"
                << "|  µOS++ SMP TEST 7                          |\n"
                << "|  USB → SD Card + UART Log + Delete Confirm |\n"
                << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start, __heap_end - __heap_start);
    exception::init();
    led_init();
  }

  [[noreturn]] static void custom_main_trampoline (void) { std::exit(os_main(0, nullptr)); }
  extern os::rtos::thread* os_main_thread;

  extern "C" int main (void) {
    using namespace os::rtos;
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        (os::rtos::thread::stack::element_t*)__fiq_stack_top,
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    os::trace::printf("\nµOS++ IIIe v" OS_STRING_RTOS_IMPL_VERSION "\n");
    os::rtos::port::scheduler::greeting();
    scheduler::initialize();
    thread::attributes attr = thread::initializer;
    attr.th_stack_size_bytes = OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES;
    os_main_thread = new thread("main", (thread::func_t)custom_main_trampoline, nullptr, attr);
    os_startup_create_thread_idle();
    scheduler::start();
    return 0;
  }
}

// The file the test writes, and the folder it lists.
static const char kReceived[] = "/sd/tests/received.dat";

static void list_tests() {
  uart_mtx.lock();
  uart::uart1 << "\n--- SD Card /sd/tests/ ---\n";
  uart_mtx.unlock();
  os::posix::directory* dir = os::posix::opendir("/sd/tests");
  if (dir == nullptr) {
    uart_mtx.lock(); uart::uart1 << "  failed to open /sd/tests\n"; uart_mtx.unlock();
    return;
  }
  for (dirent* e = dir->read(); e != nullptr; e = dir->read()) {
    char path[96];
    snprintf(path, sizeof(path), "/sd/tests/%s", e->d_name);
    struct stat st;
    const bool ok = os::posix::stat(path, &st) == 0;
    uart_mtx.lock();
    uart::uart1 << "  " << e->d_name;
    if (ok && S_ISDIR(st.st_mode)) uart::uart1 << '/';
    uart::uart1 << "  " << (int)(ok ? st.st_size : 0) << " B\n";
    uart_mtx.unlock();
  }
  dir->close();
}

extern "C" volatile uint32_t g_core_stage[3];
extern "C" void smp_install_boot_threads(void);
int usb_gadget_init(void);
extern "C" void uart_puts(const char* s);
extern "C" void uart_putc(char c);

namespace rk3506::usb::dwc2 { class Driver { public:
  static Driver& instance(void);
  void initialize(void);
  void handle_interrupt(void);
}; }

// === Thread stacks ===
static os::rtos::thread::stack::element_t usb_stack[16384];
static os::rtos::thread::stack::element_t sd_stack[16384];
static os::rtos::thread::stack::element_t led_stack[1024];

// === USB task (any core) ===
static void* usb_thread_func(void*) {
  unsigned core = port_cpu_id();
  uart_mtx.lock(); uart::uart1 << "[USB] USB task on core " << (int)core << "\n"; uart_mtx.unlock();

  auto& dwc2 = rk3506::usb::dwc2::Driver::instance();
  dwc2.initialize();
  usb_gadget_init();

  for (;;) {
    dwc2.handle_interrupt();
    os::rtos::this_thread::yield();
  }
  return nullptr;
}

// === SD task (any core) ===
static void* sd_thread_func(void*) {
  unsigned core = port_cpu_id();

  // The card's FAT volume at /sd/ (never formatted), /sd/tests/ created.
  uos_test_storage_start();
  struct statvfs vfs;
  if (os::posix::statvfs("/sd/", &vfs) != 0) {
    uart_mtx.lock(); uart::uart1 << "[SD] Mount failed: no FAT volume at /sd/ (errno " << errno << ")\n"; uart_mtx.unlock();
    test_result(false);
  }
  uart_mtx.lock(); uart::uart1 << "[SD] SD card ready on core " << (int)core << "\n"; uart_mtx.unlock();

  for (;;) {
    if (g_file_ready) {
      uart_mtx.lock();
      uart::uart1 << "\n[SD] Writing " << (int)g_file_len << " B to " << kReceived << "...\n";
      uart_mtx.unlock();

      os::posix::io* f = os::posix::open(kReceived, O_WRONLY | O_CREAT | O_TRUNC);
      bool ok = (f != nullptr);
      if (ok) {
        const ssize_t bw = f->write(g_file_buf, g_file_len);
        ok = (bw == (ssize_t)g_file_len);
        ok = (f->close() == 0) && ok;
        uart_mtx.lock(); uart::uart1 << "[SD] Written " << (int)bw << " B " << (ok ? "OK" : "FAILED") << "\n"; uart_mtx.unlock();
      } else {
        uart_mtx.lock(); uart::uart1 << "[SD] Write failed: errno " << errno << "\n"; uart_mtx.unlock();
      }

      list_tests();

      // Ask user
      uart_mtx.lock();
      uart::uart1 << "\n[CONFIRM] Delete " << kReceived << "? (y/n): ";
      uart_mtx.unlock();

      char ans;
      do { ans = uart_getc(); } while (ans != 'y' && ans != 'Y' && ans != 'n' && ans != 'N');
      uart_mtx.lock(); uart::uart1 << ans << "\n"; uart_mtx.unlock();

      if (ans == 'y' || ans == 'Y') {
        const bool del = os::posix::unlink(kReceived) == 0;
        ok = ok && del;
        uart_mtx.lock();
        uart::uart1 << "[SD] Delete: " << (del ? "OK" : "FAILED") << "\n" << "[LED] Blinking! (delete confirmed)\n";
        uart_mtx.unlock();
        g_led_blink = 1;
      } else {
        uart_mtx.lock();
        uart::uart1 << "[SD] File kept\n" << "[LED] Staying off\n";
        uart_mtx.unlock();
      }

      list_tests();

      // The verdict: one file received over USB, stored on the card, and the
      // confirm answered. A few seconds for the LED, then report and exit.
      os::rtos::sysclock.sleep_for(3000);
      test_result(ok);
    }
    os::rtos::this_thread::yield();
  }
  return nullptr;
}

// === LED task (any core) ===
static void* led_thread_func(void*) {
  unsigned core = port_cpu_id();
  uart_mtx.lock(); uart::uart1 << "[LED] LED task on core " << (int)core << "\n"; uart_mtx.unlock();

  for (;;) {
    if (g_led_blink) {
      for (int i = 0; i < 15; i++) {
        led_on(); os::rtos::sysclock.sleep_for(200);
        led_off(); os::rtos::sysclock.sleep_for(200);
      }
      g_led_blink = 0;
    }
    os::rtos::sysclock.sleep_for(500);
  }
  return nullptr;
}

// === os_main ===
int os_main (int argc, char* argv[]) {
  using namespace os::rtos;
  using uart::uart1;

  __asm__ volatile("cpsie i" ::: "memory");

  smp_install_boot_threads();
  smp::start_secondary_cores();

  int waited = 0;
  while ((g_core_stage[1] < 3 || g_core_stage[2] < 3) && waited < 3000) {
    sysclock.sleep_for(50); waited += 50;
  }
  if (g_core_stage[1] == 3 && g_core_stage[2] == 3)
    uart1 << "Both cores joined after " << waited << "ms\n";
  else
    uart1 << "TIMEOUT: c1=" << g_core_stage[1] << " c2=" << g_core_stage[2] << "\n";

  uart1 << "Starting USB → SD Card demo\n";
  uart1 << "Send: usb_test_host --big <file>\n\n";

  thread::attributes attr_usb = thread::initializer;
  attr_usb.th_stack_address = usb_stack; attr_usb.th_stack_size_bytes = sizeof(usb_stack);
  static thread usb_thread("usb", usb_thread_func, nullptr, attr_usb);
  usb_thread.cpu_affinity(0xFFFFFFFF);

  thread::attributes attr_sd = thread::initializer;
  attr_sd.th_stack_address = sd_stack; attr_sd.th_stack_size_bytes = sizeof(sd_stack);
  static thread sd_thread("sdcard", sd_thread_func, nullptr, attr_sd);
  sd_thread.cpu_affinity(0xFFFFFFFF);

  thread::attributes attr_led = thread::initializer;
  attr_led.th_stack_address = led_stack; attr_led.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread("led", led_thread_func, nullptr, attr_led);
  led_thread.cpu_affinity(0xFFFFFFFF);

  uart1 << "3 threads: usb, sdcard, led — all dynamic affinity\n";

  // The host must configure the gadget within two minutes; the SD thread
  // reports the verdict once a file has arrived and the confirm is answered.
  extern bool endpoints_enabled;
  bool configured = false;
  for (unsigned s = 0; s < 120 && !configured; ++s)
    {
      sysclock.sleep_for(1000);
      configured = endpoints_enabled;
    }
  uart_mtx.lock();
  uart1 << (configured ? "[USB] configured by the host; send: usb_test_host --big <file>\n"
                       : "[USB] NOT configured by the host within 120 s\n");
  uart_mtx.unlock();
  if (!configured)
    test_result(false);

  for (;;) sysclock.sleep_for(10000);
}

extern "C" { extern os::rtos::thread* os_idle_thread; }
namespace os { namespace rtos { namespace scheduler { extern thread* os_idle_thread_core[3]; }}}

static os::rtos::thread::stack::element_t idle1_stack[512], idle2_stack[512];
static os::rtos::thread* idle1_thread_ptr, *idle2_thread_ptr;

static void* secondary_idle_func (void*) {
  os::rtos::this_thread::thread().priority(os::rtos::thread::priority::idle);
  while (true) { __asm__ volatile("dsb" ::: "memory"); __asm__ volatile("wfi"); os::rtos::this_thread::yield(); }
  return nullptr;
}

extern "C" void smp_install_boot_threads(void) {
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
