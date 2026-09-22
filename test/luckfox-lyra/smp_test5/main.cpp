#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <ff.hpp>
#include <semihosting.hpp>

using namespace fatfs;

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

FATFS g_fs;
BYTE  g_work[4096];

static const char* fres_str(FRESULT r)
{
  switch (r)
  {
    case FR_OK: return "OK";
    case FR_DISK_ERR: return "DISK_ERR";
    case FR_INT_ERR: return "INT_ERR";
    case FR_NOT_READY: return "NOT_READY";
    case FR_NO_FILE: return "NO_FILE";
    case FR_NO_PATH: return "NO_PATH";
    case FR_INVALID_NAME: return "INVALID_NAME";
    case FR_DENIED: return "DENIED";
    case FR_EXIST: return "EXIST";
    case FR_INVALID_OBJECT: return "INVALID_OBJECT";
    case FR_WRITE_PROTECTED: return "WRITE_PROTECTED";
    case FR_INVALID_DRIVE: return "INVALID_DRIVE";
    case FR_NOT_ENABLED: return "NOT_ENABLED";
    case FR_NO_FILESYSTEM: return "NO_FILESYSTEM";
    case FR_MKFS_ABORTED: return "MKFS_ABORTED";
    case FR_TIMEOUT: return "TIMEOUT";
    case FR_LOCKED: return "LOCKED";
    case FR_NOT_ENOUGH_CORE: return "NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES: return "TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER: return "INVALID_PARAMETER";
    default: return "UNKNOWN";
  }
}

static void print_attr(uint8_t attr, uint8_t bit, char c)
{
  uart::uart1.putc((attr & bit) ? c : '-');
}

static void list_dir(const char* path, int depth)
{
  Dir dir;
  FRESULT r = dir.open(path);
  if (r != FR_OK)
  {
    uart_mtx.lock();
    uart::uart1 << "  failed to open '" << path << "': " << fres_str(r) << "\n";
    uart_mtx.unlock();
    return;
  }

  int count = 0;
  for (const auto& fno : dir)
  {
    uart_mtx.lock();
    for (int i = 0; i < depth; ++i)
      uart::uart1 << "  ";
    print_attr(fno.fattrib, AM_DIR, 'D');
    print_attr(fno.fattrib, AM_RDO, 'R');
    print_attr(fno.fattrib, AM_HID, 'H');
    print_attr(fno.fattrib, AM_SYS, 'S');
    print_attr(fno.fattrib, AM_ARC, 'A');
    uart::uart1 << ' ' << fno.fname;
    if (fno.fattrib & AM_DIR)
      uart::uart1 << '/';
    uart::uart1 << ' ';
    uart::uart1.print_dec(fno.fsize);
    uart::uart1 << " B\n";
    uart_mtx.unlock();

    if ((fno.fattrib & AM_DIR) && fno.fname[0] != '.')
    {
      char subpath[128];
      int n = snprintf(subpath, sizeof(subpath), "%s%s/", path, fno.fname);
      if (n > 0 && n < (int)sizeof(subpath))
        list_dir(subpath, depth + 1);
    }
    ++count;
  }

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

  FRESULT r = f_mount(&g_fs, "0:", 1);
  if (r == FR_NO_FILESYSTEM)
  {
    uart_mtx.lock();
    uart::uart1 << "[SD] No FAT filesystem found. Creating FAT32...\n";
    uart_mtx.unlock();
    MKFS_PARM parm;
    memset(&parm, 0, sizeof(parm));
    parm.fmt = FM_FAT32 | FM_SFD;
    parm.au_size = 0x10000;
    r = f_mkfs("0:", &parm, g_work, sizeof(g_work));
    uart_mtx.lock();
    uart::uart1 << "[SD] f_mkfs: " << fres_str(r) << "\n";
    uart_mtx.unlock();
    if (r == FR_OK)
      r = f_mount(&g_fs, "0:", 1);
  }

  if (r != FR_OK)
  {
    uart_mtx.lock();
    uart::uart1 << "[SD] Mount failed: " << fres_str(r) << "\n";
    uart_mtx.unlock();
    semihosting::report_result (false);
  }

  uart_mtx.lock();
  uart::uart1 << "[SD] Filesystem mounted OK\n";
  uart_mtx.unlock();

  DWORD free_clusters;
  FATFS* fs_ptr = &g_fs;
  r = f_getfree("0:", &free_clusters, &fs_ptr);
  if (r == FR_OK)
  {
    DWORD total_sectors = (fs_ptr->n_fatent - 2) * fs_ptr->csize;
    DWORD free_sectors = free_clusters * fs_ptr->csize;
    uart_mtx.lock();
    uart::uart1 << "[SD] Total: ~" << (total_sectors / 2048) << " MB"
                << "  Free: ~" << (free_sectors / 2048) << " MB"
                << "  Cluster: " << (fs_ptr->csize * 512) << " B\n";
    uart_mtx.unlock();
  }

  uart_mtx.lock();
  uart::uart1 << "[SD] Root directory listing:\n";
  uart_mtx.unlock();
  list_dir("0:/", 0);

  uart_mtx.lock();
  uart::uart1 << "[SD] Creating test file smp_test5.txt...\n";
  uart_mtx.unlock();
  {
    File f;
    r = f.open("0:/smp_test5.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (r == FR_OK)
    {
      UINT bw;
      f.write("Hello from Luckfox Lyra µOS++ SMP Test 5!\n", 44, &bw);
      f.write("SD card working with FatFS on 3 cores\n", 39, &bw);
      f.close();
      uart_mtx.lock();
      uart::uart1 << "[SD] Test file written OK\n";
      uart_mtx.unlock();
    }
    else
    {
      uart_mtx.lock();
      uart::uart1 << "[SD] Write failed: " << fres_str(r) << "\n";
      uart_mtx.unlock();
      semihosting::report_result (false);
    }
  }

  uart_mtx.lock();
  uart::uart1 << "[SD] Reading test file back:\n";
  uart_mtx.unlock();
  {
    File f;
    r = f.open("0:/smp_test5.txt", FA_READ);
    if (r == FR_OK)
    {
      uart_mtx.lock();
      uart::uart1 << "[SD]   Content: ";
      uart_mtx.unlock();
      char buf[128];
      UINT br;
      while (f.read(buf, sizeof(buf) - 1, &br) == FR_OK && br > 0)
      {
        buf[br] = 0;
        uart_mtx.lock();
        uart::uart1 << buf;
        uart_mtx.unlock();
      }
      uart_mtx.lock();
      uart::uart1 << "\n[SD] Readback complete\n";
      uart_mtx.unlock();
      f.close();
    }
    else
    {
      uart_mtx.lock();
      uart::uart1 << "[SD] Read failed: " << fres_str(r) << "\n";
      uart_mtx.unlock();
      semihosting::report_result (false);
    }
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
