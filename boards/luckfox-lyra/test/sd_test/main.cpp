/*
 * sd_test — Raspberry Pi Zero 2W (BCM2837, 4× Cortex-A53) SD card test.
 *
 * Two build flavours, selected by the Makefile:
 *
 *   1. Default / QEMU  (no HW=1): exercises sd::SdCard + the custom flatfs
 *      over the raw sectors of an attached disk image. The host companion
 *      tool flatfs_tool.py seeds disk.img with seed.bin and later verifies the
 *      files the firmware wrote ("host <-> device" round trip). Run with
 *      ./run.sh (raspi3b, AArch64 native).
 *
 *   2. Real hardware  (make HW=1): the microSD is the BOOT card (MBR + FAT32
 *      with config.txt / kernel8.img ...). We mount that EXISTING FAT32
 *      partition read/write through FatFs and keep every test file under a
 *      "tests" folder, so the boot files are never touched and nothing is
 *      formatted. There is no host-seeded file on a boot card, so Phase A is
 *      skipped and Phase B ("device -> device") runs against tests/.
 *
 * Run on real hardware via the 64b J-Link + OpenOCD runner (hw.sh).
 */
#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <exception_handler.hpp>
#include <sd.hpp>
#if defined(HW_BUILD)
#include <fatfs_hw.hpp>
#else
#include <flatfs.hpp>
#endif
#include <hw_result.hpp>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace os::rtos;

extern "C" unsigned port_cpu_id (void);

namespace
{

// ---------------------------------------------------------------------------
// Console (mirrors the other rpi-zero-2w tests).
// ---------------------------------------------------------------------------
void
write_str (const char* s)
{
  uart::uart1.puts (s);
}

void
write_fmt (const char* fmt, ...)
{
  char b[128];
  std::va_list args;
  va_start (args, fmt);
  int n = std::vsnprintf (b, sizeof (b), fmt, args);
  va_end (args);
  if (n < 0)
    {
      n = 0;
    }
  if (n > static_cast<int> (sizeof (b)) - 1)
    {
      n = static_cast<int> (sizeof (b)) - 1;
    }
  uart::uart1.puts (b);
}

// ---------------------------------------------------------------------------
// Deterministic pseudo-random file content. Same LCG as flatfs_tool.py, so the
// host can reproduce byte-for-byte what this firmware writes and vice-versa.
// ---------------------------------------------------------------------------
void
fill_pattern (std::uint32_t seed, std::uint8_t* buf, std::uint32_t len)
{
  std::uint32_t x = seed;
  for (std::uint32_t i = 0; i < len; ++i)
    {
      x = x * 1664525u + 1013904223u;
      buf[i] = static_cast<std::uint8_t> (x >> 24);
    }
}

// The host tool seeds the image with this file before the firmware runs.
constexpr std::uint32_t kSeedSize = 100000u;
constexpr std::uint32_t kSeedSeed = 0x51A7E51u;
constexpr const char* kSeedName = "seed.bin";

// Board-neutral on purpose: flatfs_tool.py compares this file byte for byte,
// and a host tool cannot know which board wrote the card.
constexpr const char* kHelloText
    = "Hello from flatfs on micro-os-plus-iii!\n";

struct FileSpec
{
  const char* name;
  std::uint32_t seed; // used only for the binary pattern files
  std::uint32_t size;
  bool is_text;
};

constexpr FileSpec kWriteSpecs[] = {
  { "hello.txt", 0, static_cast<std::uint32_t> (std::strlen (kHelloText)),
    true },
  { "data1.bin", 0x11111111u, 3000u, false },
  { "data2.bin", 0x22222222u, 70000u, false },
  { "big.bin",   0x33333333u, 307200u, false }, // 600 sectors: forces CMD18/25
};

bool g_all_pass = true;

void
report (const char* what, bool ok)
{
  write_fmt ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    {
      g_all_pass = false;
    }
}

// ---------------------------------------------------------------------------
// Hardware flavour: mount the EXISTING FAT32 boot partition, work in /tests.
// ---------------------------------------------------------------------------
#if defined(HW_BUILD)
void
hw_phase_format_and_write (void)
{
  write_str ("\nPhase B: device->device  (FAT32 /tests on the boot card)\n");

  // Report the MBR window first: it is resolved before FatFs is involved, so
  // it is the one diagnostic that survives a mount failure.
  std::uint32_t part_lba = 0, part_blocks = 0;
  fatfshw::partition_window (part_lba, part_blocks);
  write_fmt ("  FAT window: start LBA %u, %u sectors (%u MiB)\n", part_lba,
             part_blocks, part_blocks >> 11);

  if (!fatfshw::mount_volume ())
    {
      write_fmt ("  FAT32 mount failed: %s\n", fatfshw::last_error ());
      report ("mount FAT32 boot partition", false);
      return;
    }
  report ("mount FAT32 boot partition", true);

  const char* fat_type = "?";
  std::uint32_t spc = 0, nclust = 0;
  if (fatfshw::volume_geometry (fat_type, spc, nclust))
    {
      write_fmt ("  volume: %s, %u sectors/cluster, %u clusters\n", fat_type,
                 spc, nclust);
    }

  if (!fatfshw::ensure_tests_dir ())
    {
      write_fmt ("  mkdir tests failed: %s\n", fatfshw::last_error ());
      report ("mkdir tests", false);
      return;
    }
  report ("mkdir tests", true);

  // Clean any leftovers from a previous run, then write all files.
  for (const FileSpec& spec : kWriteSpecs)
    {
      fatfshw::remove_file (spec.name);
    }
  for (const FileSpec& spec : kWriteSpecs)
    {
      char full[64];
      std::snprintf (full, sizeof (full), "tests/%s", spec.name);
      bool wok = false;
      if (spec.is_text)
        {
          write_fmt ("  writing %s (%u B text)...\n", full, spec.size);
          wok = fatfshw::store_file (spec.name, kHelloText, spec.size);
        }
      else
        {
          write_fmt ("  writing %s (%u B pattern)...\n", full, spec.size);
          std::uint8_t* data = new std::uint8_t[spec.size];
          fill_pattern (spec.seed, data, spec.size);
          wok = fatfshw::store_file (spec.name, data, spec.size);
          delete[] data;
        }
      if (!wok)
        {
          write_fmt ("    write failed: %s\n", fatfshw::last_error ());
          report (spec.name, false);
        }
    }

  // Read each back and compare.
  write_str ("\n  read-back verification:\n");
  for (const FileSpec& spec : kWriteSpecs)
    {
      std::uint32_t size = 0;
      if (!fatfshw::file_size (spec.name, size))
        {
          report (spec.name, false);
          continue;
        }
      bool ok = (size == spec.size);
      if (ok && spec.size > 0)
        {
          std::uint8_t* got = new std::uint8_t[spec.size];
          std::uint8_t* want = new std::uint8_t[spec.size];
          if (fatfshw::load_file (spec.name, got, spec.size))
            {
              if (spec.is_text)
                {
                  ok = (std::memcmp (got, kHelloText, spec.size) == 0);
                }
              else
                {
                  fill_pattern (spec.seed, want, spec.size);
                  ok = (std::memcmp (got, want, spec.size) == 0);
                }
            }
          else
            {
              ok = false;
            }
          delete[] got;
          delete[] want;
        }
      if (!ok)
        {
          g_all_pass = false;
        }
      write_fmt ("  [%s] %s read-back verified (%u B)\n",
                 ok ? "PASS" : "FAIL", spec.name, size);
    }

  std::uint32_t total_kib = 0, free_kib = 0;
  if (fatfshw::volume_space_kib (total_kib, free_kib))
    {
      write_fmt ("  volume: %u KiB total, %u KiB free\n", total_kib, free_kib);
    }
  write_str ("\ntests/ contents:\n");
  char listing[512];
  fatfshw::list_tests_dir (listing, sizeof (listing));
  write_str (listing);
}
#endif // HW_BUILD

// ---------------------------------------------------------------------------
// QEMU flavour: flatfs over the raw sectors of the attached disk.img.
// ---------------------------------------------------------------------------
#if !defined(HW_BUILD)
// ---------------------------------------------------------------------------
// Phase A: mount the host-seeded volume and check seed.bin
// ---------------------------------------------------------------------------
void
phase_host_seed (sd::SdCard& card, flatfs::FlatFs& fs)
{
  write_str ("Phase A: host->device  (mount host-seeded volume, read seed.bin)\n");
  flatfs::FlatFs::Result r = fs.mount (card);
  if (r != flatfs::FlatFs::Result::ok)
    {
      write_fmt ("  NOTE: no flatfs volume to mount (%s) - fresh card?\n",
                 flatfs::FlatFs::result_str (r));
      return;
    }
  write_str ("  mounted volume.\n");

  std::uint32_t size = 0;
  r = fs.file_size (kSeedName, size);
  if (r != flatfs::FlatFs::Result::ok)
    {
      write_fmt ("  NOTE: %s not present (%s) - skipping.\n", kSeedName,
                 flatfs::FlatFs::result_str (r));
      return;
    }

  std::uint8_t* got = new std::uint8_t[size];
  std::uint8_t* want = new std::uint8_t[size];
  r = fs.read_file (kSeedName, got, size, nullptr);
  if (r != flatfs::FlatFs::Result::ok)
    {
      write_fmt ("  read failed: %s\n", flatfs::FlatFs::result_str (r));
      delete[] got;
      delete[] want;
      report ("host seed read", false);
      return;
    }
  fill_pattern (kSeedSeed, want, size);
  const bool ok = (size == kSeedSize) && (std::memcmp (got, want, size) == 0);
  write_fmt ("  seed.bin: %u bytes, checksum %s\n", size,
             ok ? "OK" : "MISMATCH");
  delete[] got;
  delete[] want;
  report ("host-created seed.bin verified on device", ok);
}

// ---------------------------------------------------------------------------
// Phase B: format, write the pattern files, read them back
// ---------------------------------------------------------------------------
void
phase_format_and_write (sd::SdCard& card, flatfs::FlatFs& fs)
{
  write_str ("\nPhase B: device->device  (format, write, read-back verify)\n");
  flatfs::FlatFs::Result r = fs.format (card);
  if (r != flatfs::FlatFs::Result::ok)
    {
      write_fmt ("  format failed: %s\n", flatfs::FlatFs::result_str (r));
      report ("format", false);
      return;
    }
  report ("format", true);

  // Write all files.
  for (const FileSpec& spec : kWriteSpecs)
    {
      if (spec.is_text)
        {
          write_fmt ("  writing %s (%u B text)...\n", spec.name, spec.size);
          r = fs.write_file (spec.name, kHelloText, spec.size);
        }
      else
        {
          write_fmt ("  writing %s (%u B pattern)...\n", spec.name, spec.size);
          std::uint8_t* data = new std::uint8_t[spec.size];
          fill_pattern (spec.seed, data, spec.size);
          r = fs.write_file (spec.name, data, spec.size);
          delete[] data;
        }
      if (r != flatfs::FlatFs::Result::ok)
        {
          write_fmt ("    write failed: %s\n", flatfs::FlatFs::result_str (r));
          report (spec.name, false);
        }
    }

  // Read each back and compare.
  write_str ("\n  read-back verification:\n");
  for (const FileSpec& spec : kWriteSpecs)
    {
      std::uint32_t size = 0;
      if (fs.file_size (spec.name, size) != flatfs::FlatFs::Result::ok)
        {
          report (spec.name, false);
          continue;
        }
      bool ok = (size == spec.size);
      if (ok && spec.size > 0)
        {
          std::uint8_t* got = new std::uint8_t[spec.size];
          std::uint8_t* want = new std::uint8_t[spec.size];
          if (fs.read_file (spec.name, got, spec.size, nullptr)
              == flatfs::FlatFs::Result::ok)
            {
              if (spec.is_text)
                {
                  ok = (std::memcmp (got, kHelloText, spec.size) == 0);
                }
              else
                {
                  fill_pattern (spec.seed, want, spec.size);
                  ok = (std::memcmp (got, want, spec.size) == 0);
                }
            }
          else
            {
              ok = false;
            }
          delete[] got;
          delete[] want;
        }
      if (!ok)
        {
          g_all_pass = false;
        }
      write_fmt ("  [%s] %s read-back verified (%u B)\n",
                 ok ? "PASS" : "FAIL", spec.name, size);
    }
}

void
list_volume (flatfs::FlatFs& fs)
{
  write_str ("\nVolume contents:\n");
  struct Ctx
  {
    std::uint32_t total;
    std::uint32_t free;
    std::uint32_t files;
  } st;
  fs.stat (st.total, st.free, st.files);
  write_fmt ("  card: %u sectors  free: %u sectors  files: %u\n", st.total,
             st.free, st.files);

  struct L
  {
  } dummy;
  fs.list (
      [] (const flatfs::FileEntry& f, void*) {
        write_fmt ("    %-20s  start=%8u  size=%8u\n", f.name, f.start_block,
                   f.size_bytes);
        return true;
      },
      &dummy);
}

#endif // !HW_BUILD

} // namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int
os_main (int, char*[])
{
  write_str ("\n"
             "+=================================================+\n"
             "|  " PORT_BANNER_LONG " - micro-os-plus-iii      |\n"
#if defined(HW_BUILD)
             "|  SD card (FAT32 boot partition, files in /tests)|\n"
#else
             "|  SD card (Arasan EMMC/SDHCI, PIO) + flatfs     |\n"
#endif
             "+=================================================+\n");

  // ---- SD card ----------------------------------------------------------
  write_str ("Initialising SD card via SDHCI @0x3F300000...\n");
  sd::SdCard card;
  if (!card.init ())
    {
      write_fmt ("  SD init FAILED: %s\n", card.last_error ());
      report ("SD card init", false);
      write_str ("RESULT: FAIL (no SD card)\n");
      hw_result::fail ();
      for (;;)
        {
          sysclock.sleep_for (1000);
        }
    }
  write_fmt ("  SD card ready: %s, %u sectors (~%u MiB)\n",
             card.high_capacity () ? "SDHC/SDXC (block addressing)"
                                    : "SDSC (byte addressing)",
             card.sector_count (),
             static_cast<unsigned> (card.capacity_bytes () >> 20));

#if defined(HW_BUILD)
  fatfshw::bind_card (card);
  hw_phase_format_and_write ();
#else
  flatfs::FlatFs fs;

  // ---- host -> device ---------------------------------------------------
  phase_host_seed (card, fs);

  // ---- device -> device -------------------------------------------------
  phase_format_and_write (card, fs);

  // ---- volume listing / stats -------------------------------------------
  list_volume (fs);
#endif

  write_fmt ("\nRESULT: %s\n",
#if defined(HW_BUILD)
             g_all_pass ? "PASS (SD verified)"
#else
             g_all_pass ? "PASS (SD + flatfs verified)"
#endif
                        : "FAIL (see above)");
  write_str ("-------------------------------------------------\n");
  if (g_all_pass)
    {
      hw_result::ok ();
    }
  else
    {
      hw_result::fail ();
    }

  for (;;)
    {
      sysclock.sleep_for (1000);
    }
  return 0;
}

// ---------------------------------------------------------------------------
// µOS++ startup scaffolding (single core, same pattern as smp_test0)
// ---------------------------------------------------------------------------
extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void
  os_startup_initialize_hardware (void)
  {
    uart::uart1.init ();
    os_startup_initialize_free_store (
        __heap_start, static_cast<std::size_t> (__heap_end - __heap_start));
    exception::init ();
  }

  [[noreturn]] static void
  custom_main_trampoline (void)
  {
    std::exit (os_main (0, nullptr));
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int
  main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*> (__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();

    static thread::stack::element_t main_stack[32768];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof (main_stack);
    static thread main_thread {
      "main", reinterpret_cast<thread::func_t> (custom_main_trampoline),
      nullptr, attr
    };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}
