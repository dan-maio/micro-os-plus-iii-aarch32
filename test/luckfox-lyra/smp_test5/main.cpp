#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <semihosting.hpp>

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
#include <cstdio>
#include <cstring>

static os::rtos::mutex uart_mtx("uart_lock");

static void uart_print(const char* s)
{
  uart_mtx.lock();
  uart::uart1 << s;
  uart_mtx.unlock();
}

static void uart_print_hex(uint32_t v, int digits)
{
  uart_mtx.lock();
  uart::uart1.print_hex(v, digits);
  uart_mtx.unlock();
}

static void uart_print_dec(uint32_t v)
{
  uart_mtx.lock();
  uart::uart1.print_dec(v);
  uart_mtx.unlock();
}

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
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST 5              |\n";
    uart::uart1 << "|  SD Card / LED / Logger on 3 Cores          |\n";
    uart::uart1 << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start, __heap_end - __heap_start);
    exception::init();
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

// The volume, its folders first-level and below: "D" for a folder.
static void list_dir(const char* path, int depth)
{
  os::posix::directory* dir = os::posix::opendir(path);
  if (dir == nullptr)
  {
    uart_mtx.lock();
    uart::uart1 << "  failed to open '" << path << "': errno " << errno << "\n";
    uart_mtx.unlock();
    return;
  }

  int count = 0;
  for (dirent* e = dir->read(); e != nullptr; e = dir->read())
  {
    char subpath[128];
    int n = snprintf(subpath, sizeof(subpath), "%s%s", path, e->d_name);
    struct stat st;
    const bool ok = n > 0 && n < (int)sizeof(subpath)
                    && os::posix::stat(subpath, &st) == 0;
    const bool is_dir = ok && S_ISDIR(st.st_mode);

    uart_mtx.lock();
    for (int i = 0; i < depth; ++i)
      uart::uart1 << "  ";
    uart::uart1.putc(is_dir ? 'D' : '-');
    uart::uart1 << ' ' << e->d_name;
    if (is_dir)
      uart::uart1 << '/';
    uart::uart1 << ' ';
    uart::uart1.print_dec(ok ? static_cast<uint32_t>(st.st_size) : 0u);
    uart::uart1 << " B\n";
    uart_mtx.unlock();

    if (is_dir && e->d_name[0] != '.' && n + 1 < (int)sizeof(subpath))
    {
      subpath[n] = '/';
      subpath[n + 1] = '\0';
      list_dir(subpath, depth + 1);
    }
    ++count;
  }
  dir->close();

  if (count == 0)
  {
    uart_mtx.lock();
    for (int i = 0; i < depth; ++i)
      uart::uart1 << "  ";
    uart::uart1 << "(empty)\n";
    uart_mtx.unlock();
  }
}

// --- Thread stacks ---
static os::rtos::thread::stack::element_t sd_stack[4096];
static os::rtos::thread::stack::element_t led_stack[1024];
static os::rtos::thread::stack::element_t log_stack[2048];

extern "C" volatile uint32_t g_core_stage[3];
extern "C" void smp_install_boot_threads(void);

// --- Core 2: Logger thread ---
static void* logger_thread_func(void*)
{
  unsigned core = port_cpu_id();
  unsigned count = 0;
  for (;;)
  {
    os::rtos::sysclock.sleep_for(1000);
    uart_mtx.lock();
    uart::uart1 << "[LOG t=" << (int)os::rtos::sysclock.now()
                << "ms] heartbeat=" << (int)(++count)
                << " on core " << (int)core << "\n";
    uart_mtx.unlock();
  }
  return nullptr;
}

// --- Core 1: LED blink thread ---
static void led_on()  { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0001); }
static void led_off() { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0000); }

static void* led_thread_func(void*)
{
  unsigned core = port_cpu_id();
  uart_mtx.lock();
  uart::uart1 << "[LED] blink thread started on core " << (int)core << "\n";
  uart_mtx.unlock();

  for (;;)
  {
    led_on();
    os::rtos::sysclock.sleep_for(500);
    led_off();
    os::rtos::sysclock.sleep_for(500);
  }
  return nullptr;
}

// --- Core 0: SD card thread ---
static void* sd_card_thread_func(void*)
{
  unsigned core = port_cpu_id();

  uart_mtx.lock();
  uart::uart1 << "\n[SD] SD card thread started on core " << (int)core << "\n";
  uart_mtx.unlock();

  // The card's FAT volume at /sd/ (never formatted), /sd/tests/ created.
  uos_test_storage_start();
  struct statvfs vfs;
  if (os::posix::statvfs("/sd/", &vfs) != 0)
  {
    uart_mtx.lock();
    uart::uart1 << "[SD] Mount failed: no FAT volume at /sd/ (errno " << errno << ")\n";
    uart_mtx.unlock();
    semihosting::report_result (false);
  }

  uart_mtx.lock();
  uart::uart1 << "[SD] Filesystem mounted OK\n";
  uart::uart1 << "[SD] Total: ~"
              << static_cast<int>((unsigned long long)vfs.f_blocks * vfs.f_frsize >> 20) << " MB"
              << "  Free: ~"
              << static_cast<int>((unsigned long long)vfs.f_bfree * vfs.f_frsize >> 20) << " MB\n";
  uart::uart1 << "[SD] Root directory listing:\n";
  uart_mtx.unlock();
  list_dir("/sd/", 0);

  static const char kPath[] = "/sd/tests/smp_test5.txt";
  static const char kLine1[] = "Hello from Luckfox Lyra µOS++ SMP Test 5!\n";
  static const char kLine2[] = "SD card working with FatFS on 3 cores\n";

  uart_mtx.lock();
  uart::uart1 << "[SD] Creating test file " << kPath << "...\n";
  uart_mtx.unlock();
  {
    os::posix::io* f = os::posix::open(kPath, O_WRONLY | O_CREAT | O_TRUNC);
    bool ok = (f != nullptr);
    if (ok)
    {
      ok = f->write(kLine1, std::strlen(kLine1)) == (ssize_t)std::strlen(kLine1);
      ok = ok && f->write(kLine2, std::strlen(kLine2)) == (ssize_t)std::strlen(kLine2);
      ok = (f->close() == 0) && ok;
    }
    uart_mtx.lock();
    if (ok)
      uart::uart1 << "[SD] Test file written OK\n";
    else
      uart::uart1 << "[SD] Write failed: errno " << errno << "\n";
    uart_mtx.unlock();
    if (!ok)
      semihosting::report_result (false);
  }

  uart_mtx.lock();
  uart::uart1 << "[SD] Reading test file back:\n";
  uart_mtx.unlock();
  {
    os::posix::io* f = os::posix::open(kPath, O_RDONLY);
    if (f == nullptr)
    {
      uart_mtx.lock();
      uart::uart1 << "[SD] Read failed: errno " << errno << "\n";
      uart_mtx.unlock();
      semihosting::report_result (false);
    }
    char buf[128];
    std::size_t total = 0;
    bool same = true;
    char expected[128];
    snprintf(expected, sizeof(expected), "%s%s", kLine1, kLine2);
    uart_mtx.lock();
    uart::uart1 << "[SD]   Content: ";
    uart_mtx.unlock();
    for (;;)
    {
      ssize_t br = f->read(buf, sizeof(buf) - 1);
      if (br <= 0)
        break;
      buf[br] = 0;
      if (total + br > std::strlen(expected)
          || std::memcmp(buf, expected + total, br) != 0)
        same = false;
      total += br;
      uart_mtx.lock();
      uart::uart1 << buf;
      uart_mtx.unlock();
    }
    f->close();
    same = same && total == std::strlen(expected);
    uart_mtx.lock();
    uart::uart1 << "\n[SD] Readback " << (same ? "complete, content OK" : "MISMATCH") << "\n";
    uart_mtx.unlock();
    if (!same)
      semihosting::report_result (false);
  }

  uart_mtx.lock();
  uart::uart1 << "[SD] SD card test complete on core " << (int)core << "\n";
  uart_mtx.unlock();

  // The verdict, and the stop. OpenOCD has no process status, so this line is
  // the result and the SYS_EXIT ends the run -- the hardware counterpart of
  // returning from os_main() under QEMU.
  semihosting::report_result (true);
}

// --- os_main: boot cores, create threads with affinity ---
int os_main (int argc, char* argv[])
{
  using namespace os::rtos;
  using uart::uart1;

  __asm__ volatile("cpsie i" ::: "memory");

  // Init LED GPIO
  using namespace rk3506;
  cru::CLKGATE_CON22::clear_bits(0x0002);
  cru::CLKGATE_CON03::clear_bits(0x0300);
  cru::SOFTRST_CON22::clear_bits(0x0002);
  cru::SOFTRST_CON03::clear_bits(0x0300);
  gpio1_ioc::GPIO1A_IOMUX_SEL0::write(0x000F, 0x0000);
  gpio1::SWPORT_DDR_L::write(0x0001, 0x0001);

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

  // Create SD card thread (Core 0)
  thread::attributes attr_sd = thread::initializer;
  attr_sd.th_stack_address = sd_stack;
  attr_sd.th_stack_size_bytes = sizeof(sd_stack);
  static thread sd_thread("sdcard", sd_card_thread_func, nullptr, attr_sd);
  sd_thread.cpu_affinity(1u << 0);

  // Create LED blink thread (Core 1)
  thread::attributes attr_led = thread::initializer;
  attr_led.th_stack_address = led_stack;
  attr_led.th_stack_size_bytes = sizeof(led_stack);
  static thread led_thread("led", led_thread_func, nullptr, attr_led);
  led_thread.cpu_affinity(1u << 1);

  // Create logger thread (Core 2)
  thread::attributes attr_log = thread::initializer;
  attr_log.th_stack_address = log_stack;
  attr_log.th_stack_size_bytes = sizeof(log_stack);
  static thread log_thread("logger", logger_thread_func, nullptr, attr_log);
  log_thread.cpu_affinity(1u << 2);

  uart1 << "3 threads created: sd(0), led(1), logger(2)\n";

  for (;;)
    sysclock.sleep_for(10000);
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
