/*
 * usb_test — Raspberry Pi Zero 2W (BCM2837) USB gadget file-transfer test.
 *
 * A genuine µOS++ SMP application. After the board is up and the DWC2 core is
 * in device mode, four application threads run alongside the USB service
 * thread:
 *
 *   usb     (core 0)  runs usb_dwc2::service_thread_body(): the ISR-driven
 *                     service loop, and - once the host configures us - the
 *                     command loop that reads files off the bulk OUT endpoint
 *                     and writes them to the SD card through a Sink.
 *   led               heartbeats the user LED.
 *   uart              the ONLY writer of the PL011 console: drains a message
 *                     queue of file chunks and status text.
 *   semihost          the ONLY writer of the ARM semihosting channel: same
 *                     queue discipline, for the OpenOCD/J-Link run.
 *
 * Two unpinned worker threads keep every core scheduling while USB traffic
 * flows, and report a per-core iteration count for the SMP tally.
 *
 * The received file is stored through the selected Sink AND streamed to the
 * UART and semihosting threads, hexdumped, so the file is observable on the
 * console as well as on the SD card.
 *
 * QEMU models the DWC2 core as a host controller only, so init() reports
 * no_device_mode and this test prints RESULT: SKIP there; the application
 * threads never start.
 */
#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <semihosting.hpp>
#include <led.hpp>
#include <exception_handler.hpp>
#include <bcm2837.hpp>
#include <mailbox.hpp>
#include <usb_dwc2.hpp>
#include <sd.hpp>
#include <flatfs.hpp>
#include <smp.hpp>
#include <hw_result.hpp>

#include "protocol.hpp"
#include "sink.hpp"

#if defined(HW_BUILD)
#include <fatfs_hw.hpp>
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see test_smpl/common/src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

using namespace os::rtos;

extern "C" unsigned port_cpu_id (void);

namespace
{

// ---------------------------------------------------------------------------
// Console helpers (used before the writer threads exist).
// ---------------------------------------------------------------------------
void
write_str (const char* s)
{
  uart::uart1.puts (s);
}

void
write_fmt (const char* fmt, ...)
{
  char b[160];
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
// Inter-thread messages
// ---------------------------------------------------------------------------
inline constexpr unsigned kChunkData = 128u;
inline constexpr std::uint8_t kMsgChunk = 0u; // file bytes -> hexdump
inline constexpr std::uint8_t kMsgText  = 1u; // NUL-terminated status line
inline constexpr std::uint8_t kMsgFlush = 2u; // drain sentinel

struct OutMsg
{
  std::uint8_t kind;
  char name[flatfs::kNameMax + 1];
  std::uint32_t offset;
  std::uint32_t len;
  std::uint8_t data[kChunkData];
};

message_queue* g_uart_q = nullptr;
message_queue* g_semi_q = nullptr;
semaphore_binary* g_uart_flushed = nullptr;
semaphore_binary* g_semi_flushed = nullptr;

usbtest::Sink* g_sink = nullptr;
std::uint32_t g_file_max = 0u;

// SMP load tally: each core writes only its own slot.
volatile std::uint32_t g_iters[OS_NCPU] = {};

// ---------------------------------------------------------------------------
// Output threads
// ---------------------------------------------------------------------------
// Build "usb-rx <name> <offset>  xx xx ..\n" into `out` (cap bytes).
unsigned
build_hexdump (const OutMsg& m, char* out, unsigned cap)
{
  unsigned n = 0u;
  n += static_cast<unsigned> (std::snprintf (out + n, (n < cap) ? cap - n : 0u,
                                             "usb-rx %s %08lx  ", m.name,
                                             static_cast<unsigned long> (m.offset)));
  for (std::uint32_t i = 0; i < m.len && n + 4u < cap; ++i)
    {
      n += static_cast<unsigned> (std::snprintf (out + n, cap - n, "%02x ",
                                                 static_cast<unsigned> (m.data[i])));
    }
  if (n + 1u < cap)
    {
      out[n++] = '\n';
    }
  out[n] = '\0';
  return n;
}

void*
uart_thread_fn (void*)
{
  for (;;)
    {
      OutMsg m;
      if (g_uart_q->receive (&m, sizeof (m)) != result::ok)
        {
          continue;
        }
      if (m.kind == kMsgFlush)
        {
          g_uart_flushed->post ();
          continue;
        }
      if (m.kind == kMsgText)
        {
          uart::uart1.puts_uart (reinterpret_cast<const char*> (m.data));
          continue;
        }
      char line[8u * kChunkData + 64u];
      build_hexdump (m, line, sizeof (line));
      uart::uart1.puts_uart (line);
    }
  return nullptr;
}

void*
semi_thread_fn (void*)
{
  for (;;)
    {
      OutMsg m;
      if (g_semi_q->receive (&m, sizeof (m)) != result::ok)
        {
          continue;
        }
      if (m.kind == kMsgFlush)
        {
          g_semi_flushed->post ();
          continue;
        }
#if defined(SEMIHOST)
      if (m.kind == kMsgText)
        {
          semihosting::write_str (reinterpret_cast<const char*> (m.data));
          continue;
        }
      char line[8u * kChunkData + 64u];
      build_hexdump (m, line, sizeof (line));
      semihosting::write_str (line);
#else
      // No debugger: a semihosting HLT would halt the core. Drain and discard,
      // still answering the flush sentinel so the USB thread can finish.
      (void)m;
#endif
    }
  return nullptr;
}

void*
led_thread_fn (void*)
{
  // A burst of kLedBlinks blinks, then a dark gap, repeating. The three
  // numbers are Makefile knobs (LED_BLINKS / LED_ON_MS / LED_OFF_MS /
  // LED_GAP_MS); each half-cycle is clamped to sysclock's 1 ms tick.
  constexpr unsigned kLedBlinks = LED_BLINKS;
  constexpr unsigned kOnMs = (LED_ON_MS != 0u) ? LED_ON_MS : 1u;
  constexpr unsigned kOffMs = (LED_OFF_MS != 0u) ? LED_OFF_MS : 1u;

  for (;;)
    {
      for (unsigned i = 0; i < kLedBlinks; ++i)
        {
          led::set (true);
          sysclock.sleep_for (kOnMs);
          led::set (false);
          sysclock.sleep_for (kOffMs);
        }
      sysclock.sleep_for (LED_GAP_MS);
    }
  return nullptr;
}

void*
worker_thread_fn (void*)
{
  for (;;)
    {
      const unsigned c = port_cpu_id ();
      if (c < OS_NCPU)
        {
          g_iters[c] = g_iters[c] + 1u;
        }
      this_thread::yield ();
      if (((g_iters[c < OS_NCPU ? c : 0u] & 0x3FFu) == 0u))
        {
          sysclock.sleep_for (1);
        }
    }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Command loop — runs in the USB service thread once the host configures us.
// ---------------------------------------------------------------------------
// Console lines the queues had no room for. Diagnostics must never throttle
// the protocol: the USB service thread posts these between commands, and the
// semihosting consumer drains one debug trap at a time. A BLOCKING post means
// the thread is not back in the command loop and bulk OUT is not re-armed, so
// the host's NEXT command times out -- which is how a fully completed PUT came
// to look like a device hang. Dropping a diagnostic line is the right trade:
// the verdict is built from the CRC tally, never from the dump.
std::uint32_t g_console_dropped = 0u;

void
post_console (const OutMsg& m)
{
  if (g_uart_q->try_send (&m, sizeof (m)) != result::ok)
    {
      ++g_console_dropped;
    }
  if (g_semi_q->try_send (&m, sizeof (m)) != result::ok)
    {
      ++g_console_dropped;
    }
}

void
log_line (const char* text)
{
  OutMsg m;
  std::memset (&m, 0, sizeof (m));
  m.kind = kMsgText;
  std::strncpy (reinterpret_cast<char*> (m.data), text, kChunkData - 1u);
  m.data[kChunkData - 1u] = '\0';
  post_console (m);
}

// ---------------------------------------------------------------------------
// Storage, and the thread that owns it
// ---------------------------------------------------------------------------
// SMP rule S7: storage has exactly ONE writer. Every byte that reaches the
// card goes through sd_thread_fn(); nothing else touches the card, the FatFs
// volume or the flatfs volume. The requester blocks until the result comes
// back, so the protocol reply still carries the real status of the write
// rather than an optimistic guess, and a GET can hand back real bytes.
//
// Requests are serialised by construction: the main thread issues the mount
// before the USB service thread exists, and from then on the USB service
// thread is the only requester. One in-flight request and a single completion
// semaphore are therefore enough.
enum class SdOp : std::uint8_t
{
  mount,
  put,
  get,
  list
};

struct SdReq
{
  SdOp op;
  const char* name;
  const void* data;          // put: bytes to write
  void* buf;                 // get: destination
  std::uint32_t len;         // put: byte count; get: buffer capacity
  flatfs::FileVisitor visit; // list
  void* visit_ctx;
};

message_queue* g_sd_q = nullptr;
semaphore_binary* g_sd_done = nullptr;
volatile std::uint32_t g_sd_result = 0u;
volatile std::uint32_t g_sd_out_len = 0u;

sd::SdCard g_card;
usbtest::RamUartSink g_ram_sink;
#if defined(HW_BUILD)
usbtest::FatFsSink g_fat_sink;
#else
flatfs::FlatFs g_fs;
usbtest::FlatFsSink g_flat_sink (g_fs);
#endif

usbtest::Sink::Result
sd_call (const SdReq& r)
{
  g_sd_q->send (&r, sizeof (r));
  g_sd_done->wait ();
  return static_cast<usbtest::Sink::Result> (g_sd_result);
}

usbtest::Sink::Result
sd_put (const char* n, const void* data, std::uint32_t len)
{
  SdReq r{};
  r.op = SdOp::put;
  r.name = n;
  r.data = data;
  r.len = len;
  return sd_call (r);
}

usbtest::Sink::Result
sd_get (const char* n, void* buf, std::uint32_t cap, std::uint32_t& out_len)
{
  SdReq r{};
  r.op = SdOp::get;
  r.name = n;
  r.buf = buf;
  r.len = cap;
  const usbtest::Sink::Result res = sd_call (r);
  out_len = g_sd_out_len;
  return res;
}

usbtest::Sink::Result
sd_list (flatfs::FileVisitor v, void* ctx)
{
  SdReq r{};
  r.op = SdOp::list;
  r.visit = v;
  r.visit_ctx = ctx;
  return sd_call (r);
}

// Runs in the SD thread only.
bool
sd_mount_storage ()
{
  if (!g_card.init ())
    {
      log_line ("storage: no SD card detected\n");
      return false;
    }

  char line[128];
#if defined(HW_BUILD)
  // The card is the BOOT card: MBR + FAT32 with config.txt and the kernel
  // image at its root. MOUNT it, never format it, and keep every file under
  // tests/ so nothing at the root is touched. (flatfs cannot be used here at
  // all - its superblock is LBA 0, which on this card is the MBR.)
  fatfshw::bind_card (g_card);

  // Report the MBR window first: it is resolved before FatFs is involved, so
  // it survives a mount failure and is the key diagnostic on a strange card.
  std::uint32_t part_lba = 0u, part_blocks = 0u;
  fatfshw::partition_window (part_lba, part_blocks);
  std::snprintf (line, sizeof (line),
                 "storage: FAT window LBA %lu, %lu sectors (%lu MiB)\n",
                 (unsigned long)part_lba, (unsigned long)part_blocks,
                 (unsigned long)(part_blocks >> 11));
  log_line (line);

  if (!g_fat_sink.mount ())
    {
      std::snprintf (line, sizeof (line), "storage: FAT32 mount failed: %s\n",
                     fatfshw::last_error ());
      log_line (line);
      return false;
    }
  const char* fat_type = "?";
  std::uint32_t spc = 0u, nclust = 0u;
  if (fatfshw::volume_geometry (fat_type, spc, nclust))
    {
      std::snprintf (line, sizeof (line),
                     "storage: %s, %lu sectors/cluster, %lu clusters\n",
                     fat_type, (unsigned long)spc, (unsigned long)nclust);
      log_line (line);
    }
  std::uint32_t total_kib = 0u, free_kib = 0u;
  if (fatfshw::volume_space_kib (total_kib, free_kib))
    {
      std::snprintf (line, sizeof (line),
                     "storage: %lu KiB total, %lu KiB free\n",
                     (unsigned long)total_kib, (unsigned long)free_kib);
      log_line (line);
    }
  g_sink = &g_fat_sink;
#else
  // QEMU: a raw disk image is attached, so flatfs owns the whole device.
  if (g_fs.mount (g_card) != flatfs::FlatFs::Result::ok)
    {
      log_line ("storage: flatfs mount failed\n");
      return false;
    }
  (void)line;
  g_sink = &g_flat_sink;
#endif
  g_file_max = 1u << 20;
  return true;
}

// One line per file, emitted through the console queues by the SD thread.
bool
log_list_visit (const flatfs::FileEntry& e, void* ctx)
{
  unsigned* n = static_cast<unsigned*> (ctx);
  char line[96];
  std::snprintf (line, sizeof (line), "  %-12s  %8lu\n", e.name,
                 (unsigned long)e.size_bytes);
  log_line (line);
  ++*n;
  return true;
}

// List the folder this run wrote into, the way sd_test and the other SD tests
// end. Called from the USB service thread; the walk itself runs in the SD
// thread, which is the only thing allowed to touch the volume.
void
log_storage_listing ()
{
  char hdr[96];
  std::snprintf (hdr, sizeof (hdr), "\n%s listing:\n", g_sink->name ());
  log_line (hdr);
  unsigned n = 0u;
  (void)sd_list (log_list_visit, &n);
  if (n == 0u)
    {
      log_line ("  (empty)\n");
    }
  else
    {
      std::snprintf (hdr, sizeof (hdr), "  %u file(s)\n", n);
      log_line (hdr);
    }
}

void*
sd_thread_fn (void*)
{
  for (;;)
    {
      SdReq r;
      if (g_sd_q->receive (&r, sizeof (r)) != result::ok)
        {
          continue;
        }
      usbtest::Sink::Result res = usbtest::Sink::Result::io_error;
      std::uint32_t out = 0u;
      switch (r.op)
        {
        case SdOp::mount:
          if (!sd_mount_storage ())
            {
              g_sink = &g_ram_sink;
              g_file_max = usbtest::kRamFileMax;
            }
          {
            char line[96];
            std::snprintf (line, sizeof (line), "storage: %s\n",
                           g_sink->name ());
            log_line (line);
          }
          res = usbtest::Sink::Result::ok;
          break;

        case SdOp::put:
          res = g_sink->put (r.name, r.data, r.len);
          break;

        case SdOp::get:
          res = g_sink->get (r.name, r.buf, r.len, out);
          break;

        case SdOp::list:
          res = g_sink->list (r.visit, r.visit_ctx);
          break;
        }
      g_sd_out_len = out;
      g_sd_result = static_cast<std::uint32_t> (res);
      g_sd_done->post ();
    }
  return nullptr;
}

void
stream_file (const char* name, const std::uint8_t* data, std::uint32_t len)
{
  char banner[96];
  std::snprintf (banner, sizeof (banner), "---- BEGIN %s (%lu bytes) ----\n", name,
                 static_cast<unsigned long> (len));
  log_line (banner);

  for (std::uint32_t off = 0u; off < len; off += kChunkData)
    {
      OutMsg m;
      std::memset (&m, 0, sizeof (m));
      m.kind = kMsgChunk;
      std::strncpy (m.name, name, flatfs::kNameMax);
      m.name[flatfs::kNameMax] = '\0';
      m.offset = off;
      m.len = (len - off < kChunkData) ? (len - off) : kChunkData;
      std::memcpy (m.data, data + off, m.len);
      post_console (m);
    }

  std::snprintf (banner, sizeof (banner), "---- END %s crc32=%08lx ----\n", name,
                 static_cast<unsigned long> (usbtest::crc32 (data, len)));
  log_line (banner);
}

void
send_reply (std::uint32_t status, const void* payload, std::uint32_t len)
{
  usbtest::Reply r;
  r.magic = usbtest::kRspMagic;
  r.status = status;
  r.length = len;
  r.crc32 = (len != 0u) ? usbtest::crc32 (payload, len) : 0u;
  usb_dwc2::ep_write (&r, sizeof (r));
  if (len != 0u)
    {
      usb_dwc2::ep_write (payload, len);
    }
}

void
flush_consoles ()
{
  OutMsg s;
  std::memset (&s, 0, sizeof (s));
  s.kind = kMsgFlush;
  g_uart_q->send (&s, sizeof (s));
  g_semi_q->send (&s, sizeof (s));
  g_uart_flushed->wait ();
  g_semi_flushed->wait ();
}

[[noreturn]] void
finish (std::uint32_t commands, std::uint32_t bytes_in, std::uint32_t bytes_out,
        std::uint32_t crc_errors)
{
  char line[128];
  std::snprintf (line, sizeof (line),
                 "commands=%lu bytes_in=%lu bytes_out=%lu crc_errors=%lu"
                 " console_dropped=%lu\n",
                 static_cast<unsigned long> (commands),
                 static_cast<unsigned long> (bytes_in),
                 static_cast<unsigned long> (bytes_out),
                 static_cast<unsigned long> (crc_errors),
                 static_cast<unsigned long> (g_console_dropped));
  log_line (line);

  // What actually landed in the folder this run wrote into.
  log_storage_listing ();

  unsigned active = 0u;
  for (unsigned c = 0u; c < OS_NCPU; ++c)
    {
      std::snprintf (line, sizeof (line), "  core%u iterations=%lu\n", c,
                     static_cast<unsigned long> (g_iters[c]));
      log_line (line);
      if (g_iters[c] != 0u)
        {
          ++active;
        }
    }

  const bool ok = (crc_errors == 0u) && (commands != 0u) && (active >= 3u);
  log_line (ok ? "RESULT: PASS\n" : "RESULT: FAIL (see tally)\n");
  flush_consoles ();
  if (ok)
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
}

struct ListCtx
{
  std::uint8_t* out;
  std::uint32_t cap;
  std::uint32_t used;
};

bool
list_visit (const flatfs::FileEntry& e, void* ctx)
{
  ListCtx* c = static_cast<ListCtx*> (ctx);
  const std::uint32_t need = static_cast<std::uint32_t> (std::strlen (e.name)) + 1u
                             + 4u;
  if (c->used + need > c->cap)
    {
      return false;
    }
  std::size_t nl = std::strlen (e.name) + 1u;
  std::memcpy (c->out + c->used, e.name, nl);
  c->used += static_cast<std::uint32_t> (nl);
  c->out[c->used++] = static_cast<std::uint8_t> (e.size_bytes & 0xFFu);
  c->out[c->used++] = static_cast<std::uint8_t> ((e.size_bytes >> 8) & 0xFFu);
  c->out[c->used++] = static_cast<std::uint8_t> ((e.size_bytes >> 16) & 0xFFu);
  c->out[c->used++] = static_cast<std::uint8_t> ((e.size_bytes >> 24) & 0xFFu);
  return true;
}

void
command_loop (void*)
{
  static std::uint8_t filebuf[1u << 20]; // 1 MiB working buffer
  std::uint32_t commands = 0u;
  std::uint32_t bytes_in = 0u;
  std::uint32_t bytes_out = 0u;
  std::uint32_t crc_errors = 0u;

  log_line ("USB device configured by host\n");

  for (;;)
    {
      usbtest::Command cmd;
      const int n = usb_dwc2::ep_read (&cmd, sizeof (cmd));
      if (n != static_cast<int> (sizeof (cmd)))
        {
          continue;
        }
      if (cmd.magic != usbtest::kCmdMagic)
        {
          continue;
        }
      ++commands;

      char name[flatfs::kNameMax + 1];
      std::size_t ni = 0u;
      for (; ni < flatfs::kNameMax && cmd.name[ni] != '\0'; ++ni)
        {
          name[ni] = cmd.name[ni];
        }
      name[ni] = '\0';

      switch (static_cast<usbtest::Op> (cmd.op))
        {
        case usbtest::Op::ping:
          send_reply (0u, nullptr, 0u);
          if ((cmd.flags & 1u) != 0u)
            {
              finish (commands, bytes_in, bytes_out, crc_errors);
            }
          break;

        case usbtest::Op::put:
          {
            std::uint32_t len = cmd.length;
            if (len > sizeof (filebuf))
              {
                len = sizeof (filebuf);
              }
            int got = usb_dwc2::ep_read (filebuf, len);
            if (got < 0)
              {
                got = 0;
              }
            const std::uint32_t ulen = static_cast<std::uint32_t> (got);
            const usbtest::Sink::Result r = sd_put (name, filebuf, ulen);

            // Answer FIRST, then talk to the consoles. The store result is
            // already known here, and everything below is diagnostics: a
            // hexdump and a folder listing, each line a queue post that
            // blocks once the 16-deep queue is full and is drained one
            // semihosting trap at a time. Under a JTAG probe that is slow
            // enough to outlast the host's reply timeout, so a reply sent
            // after the logging made a completed PUT look like a hang.
            send_reply (static_cast<std::uint32_t> (r), nullptr, 0u);

            if (r == usbtest::Sink::Result::ok)
              {
                bytes_in += ulen;
#if defined(HW_BUILD)
                // FatFs is built without long-name support, so say what the
                // file is really called on the card instead of renaming it
                // behind the user's back.
                if (g_sink == &g_fat_sink)
                  {
                    char leaf[13];
                    usbtest::FatFsSink::to_83 (name, leaf);
                    if (std::strcmp (leaf, name) != 0)
                      {
                        char note[96];
                        std::snprintf (note, sizeof (note),
                                       "stored: %s -> tests/%s (8.3)\n", name,
                                       leaf);
                        log_line (note);
                      }
                  }
#endif
                stream_file (name, filebuf, ulen); // SD + console, both paths
                // "where the write was done", right after it was done. The
                // run only reaches finish() on a terminating PING, so without
                // this a plain send_file.py would never show the folder.
                log_storage_listing ();
              }
          }
          break;

        case usbtest::Op::get:
          {
            std::uint32_t out_len = 0u;
            const usbtest::Sink::Result r
                = sd_get (name, filebuf, sizeof (filebuf), out_len);
            if (r == usbtest::Sink::Result::ok)
              {
                bytes_out += out_len;
                send_reply (0u, filebuf, out_len);
              }
            else
              {
                send_reply (static_cast<std::uint32_t> (r), nullptr, 0u);
              }
          }
          break;

        case usbtest::Op::list:
          {
            ListCtx ctx{ filebuf, static_cast<std::uint32_t> (sizeof (filebuf)), 0u };
            const usbtest::Sink::Result r = sd_list (list_visit, &ctx);
            if (r == usbtest::Sink::Result::ok)
              {
                send_reply (0u, filebuf, ctx.used);
              }
            else
              {
                send_reply (static_cast<std::uint32_t> (r), nullptr, 0u);
              }
          }
          break;

        default:
          send_reply (static_cast<std::uint32_t> (usbtest::Sink::Result::io_error),
                      nullptr, 0u);
          break;
        }
    }
}

// ---------------------------------------------------------------------------
// Thread startup
// ---------------------------------------------------------------------------
// Per-core idle threads for cores 1..3. Installed into os_idle_thread_core[]
// BEFORE smp::start_secondary_cores() releases the cores; each released core
// enters the scheduler and adopts its own idle thread.

thread::stack::element_t usb_stack[2048];
thread::stack::element_t led_stack[512];
thread::stack::element_t uart_stack[1024];
thread::stack::element_t semi_stack[1024];
thread::stack::element_t mon_stack[1024];
// FatFs puts a FIL (with its 512-byte sector buffer) on the stack.
thread::stack::element_t sd_stack[2048];
thread::stack::element_t work_stack[2][512];

// Print the USB bring-up counters every 2 s until the host configures us.
// Hardware bring-up aid: it is silent once configured.
void*
monitor_thread_fn (void*)
{
  for (;;)
    {
      sysclock.sleep_for (2000);
      if (usb_dwc2::configured ())
        {
          continue;
        }
      const volatile usb_dwc2::Debug& d = usb_dwc2::debug ();
      char b[256];
      std::snprintf (b, sizeof (b),
                     "usb_dbg isr=%lu off=%lu sts=0x%08lx rst=%lu enum=%lu rx=%lu "
                     "setup=%lu bm=0x%02lx req=0x%02lx wl=%lu e0in=%lu e0out=%lu "
                     "iep=%lu oep=%lu svc=%lu tx0=0x%08lx txl=%lu "
                     "die0=0x%08lx doe0=0x%08lx res=%lu dsts=0x%08lx\n",
                     (unsigned long)d.isr_calls, (unsigned long)d.isr_offcore,
                     (unsigned long)d.gintsts, (unsigned long)d.usbrst,
                     (unsigned long)d.enumdone, (unsigned long)d.rx_pkts,
                     (unsigned long)d.setups, (unsigned long)d.last_bmrt,
                     (unsigned long)d.last_brequest,
                     (unsigned long)d.last_wlength, (unsigned long)d.ep0_in,
                     (unsigned long)d.ep0_out, (unsigned long)d.iep,
                     (unsigned long)d.oep, (unsigned long)d.service_loops,
                     (unsigned long)d.ep0_tx0, (unsigned long)d.ep0_txlen,
                     (unsigned long)d.last_diepint0,
                     (unsigned long)d.last_doepint0,
                     (unsigned long)d.ep0_in_residue, (unsigned long)d.dsts);
      uart::uart1.puts (b);

      char h[192];
      unsigned hn = static_cast<unsigned> (
          std::snprintf (h, sizeof (h), "usb_seq"));
      for (unsigned k = 0; k < 4u; ++k)
        {
          const unsigned i = (d.setup_idx - 4u + k) & 7u;
          const std::uint32_t e = d.setup_hist[i];
          hn += static_cast<unsigned> (std::snprintf (
              h + hn, sizeof (h) - hn, " %02lx/%02lx/%lu",
              static_cast<unsigned long> ((e >> 24) & 0xFFu),
              static_cast<unsigned long> ((e >> 16) & 0xFFu),
              static_cast<unsigned long> (e & 0xFFFFu)));
        }
      std::snprintf (h + hn, sizeof (h) - hn, "\n");
      uart::uart1.puts (h);

      const usb_dwc2::RegSnapshot s = usb_dwc2::snapshot ();
      std::snprintf (b, sizeof (b),
                     "usb_reg diepctl0=0x%08lx dieptsiz0=0x%08lx diepint0=0x%08lx "
                     "doepctl0=0x%08lx doeptsiz0=0x%08lx doepint0=0x%08lx "
                     "dcfg=0x%08lx dctl=0x%08lx dsts=0x%08lx "
                     "gintsts=0x%08lx gintmsk=0x%08lx gahbcfg=0x%08lx\n",
                     (unsigned long)s.diepctl0, (unsigned long)s.dieptsiz0,
                     (unsigned long)s.diepint0, (unsigned long)s.doepctl0,
                     (unsigned long)s.doeptsiz0, (unsigned long)s.doepint0,
                     (unsigned long)s.dcfg, (unsigned long)s.dctl,
                     (unsigned long)s.dsts, (unsigned long)s.gintsts,
                     (unsigned long)s.gintmsk, (unsigned long)s.gahbcfg);
      uart::uart1.puts (b);
    }
  return nullptr;
}

void
start_thread (thread::stack::element_t* stack, std::size_t bytes, const char* name,
              thread::func_t fn, unsigned affinity,
              thread::priority_t prio = thread::priority::normal)
{
  thread::attributes a = thread::initializer;
  a.th_stack_address = stack;
  a.th_stack_size_bytes = bytes;
  a.th_priority = prio;
  thread* t = new thread (name, fn, nullptr, a);
  if (t != nullptr && affinity != 0u)
    {
      t->cpu_affinity (affinity);
    }
}

void*
usb_service_trampoline (void*)
{
  usb_dwc2::service_thread_body (); // never returns
  return nullptr;
}

} // anonymous namespace

// Create the per-core idle threads for cores 1..3 (the port's weak default
// only knows core 0). Must run before smp::start_secondary_cores().
// ---------------------------------------------------------------------------
// os_main
// ---------------------------------------------------------------------------
int
os_main (int argc, char* argv[])
{
  (void)argc;
  (void)argv;

  write_str ("\n=== usb_test on " PORT_BANNER_LONG " ===\n");

  const bool powered = mailbox::set_power_state (mailbox::kPowerDeviceUsb, true);
  write_fmt ("USB power domain: %s (code=0x%08lx state=0x%08lx alias=%u)\n",
             powered ? "on" : "FAILED", (unsigned long)mailbox::last_code (),
             (unsigned long)mailbox::last_state (),
             mailbox::last_used_alias () ? 1u : 0u);

  const usb_dwc2::HwInfo hw = usb_dwc2::probe_hw ();
  write_fmt ("DWC2 GSNPSID: 0x%08lx  GHWCFG2: 0x%08lx\n",
             (unsigned long)hw.snpsid, (unsigned long)hw.hwcfg2);
  write_fmt ("DWC2 GHWCFG3: 0x%08lx  GHWCFG4: 0x%08lx\n",
             (unsigned long)hw.hwcfg3, (unsigned long)hw.hwcfg4);

  // ---------------------------------------------------------------------
  // Everything slow happens HERE, before the USB core is connected. Once
  // init() clears DCTL.SDIS the host may start enumerating immediately, and
  // it must find EP0 armed. In particular the SD probe (card.init/mount) can
  // block for a long time, so it must NOT run between init() and the service
  // thread's first arm_ep0_out(): that was the enumeration hang.
  // ---------------------------------------------------------------------

  // Release cores 1..3 so the load generators and the console/storage threads
  // genuinely spread over every core (smp_test2 pattern: the per-core idle
  // threads are installed first, then each core is released and waits for its
  // first tick before the next). Until this ran, everything was on core 0.
  smp_install_boot_threads ();
  smp::start_secondary_cores ();

  // Inter-thread channels.
  // 64 deep, not 16: a 511-byte hexdump plus a folder listing fits without
  // dropping anything, while try_send() keeps a 1 MiB dump from ever stalling
  // the protocol.
  g_uart_q = new message_queue ("uartq", 64u, sizeof (OutMsg));
  g_semi_q = new message_queue ("semiq", 64u, sizeof (OutMsg));
  g_uart_flushed = new semaphore_binary ("uartf", 0u);
  g_semi_flushed = new semaphore_binary ("semif", 0u);
  g_sd_q = new message_queue ("sdq", 4u, sizeof (SdReq));
  g_sd_done = new semaphore_binary ("sddone", 0u);

  led::init ();

  // Console writers first, so the queues are drained as soon as data lands.
  start_thread (uart_stack, sizeof (uart_stack), "uart", uart_thread_fn, 0u);
  start_thread (semi_stack, sizeof (semi_stack), "semi", semi_thread_fn, 0u);

  // The heartbeat LED starts BEFORE the blocking SD mount below and runs at
  // above-normal priority so the two busy-spin load generators cannot starve
  // it. It is left unpinned: cores 1-3 are live by now (§10.1), so it can run
  // on whichever core is free instead of queueing behind the core-0 USB loop.
  start_thread (led_stack, sizeof (led_stack), "led", led_thread_fn, 0u,
                thread::priority::above_normal);

  // Storage next, and synchronously. Probing and mounting the card is by far
  // the slowest thing this program does, and it MUST complete before
  // usb_dwc2::init() clears DCTL.SDIS - a card probe running between init()
  // and the service thread's first arm_ep0_setup() was the original
  // enumeration hang. Doing it here, inside the thread that owns the card,
  // keeps that ordering while still making the SD thread the single writer.
  start_thread (sd_stack, sizeof (sd_stack), "sd", sd_thread_fn, 0u);
  {
    SdReq mount_req{};
    mount_req.op = SdOp::mount;
    (void)sd_call (mount_req);
  }
  start_thread (mon_stack, sizeof (mon_stack), "mon", monitor_thread_fn, 0u);

  // Unpinned load generators: they keep every core scheduling while USB
  // traffic flows, and prove the SMP path is exercised.
  start_thread (work_stack[0], sizeof (work_stack[0]), "w0", worker_thread_fn, 0u);
  start_thread (work_stack[1], sizeof (work_stack[1]), "w1", worker_thread_fn, 0u);

  usb_dwc2::set_configured_callback (command_loop, nullptr);

  // Bring up the USB core LAST and start the service thread with nothing slow
  // in between. start_thread() only queues it; the main thread then sleeps,
  // which lets it run and arm EP0 within the first scheduler tick.
  const usb_dwc2::InitResult ir = usb_dwc2::init ();
  if (ir != usb_dwc2::InitResult::ok)
    {
      write_fmt ("\nRESULT: SKIP (%s)\n", usb_dwc2::init_result_str (ir));
      hw_result::ok ();
      for (;;)
        {
          sysclock.sleep_for (1000);
        }
    }
  start_thread (usb_stack, sizeof (usb_stack), "usb", usb_service_trampoline, 1u << 0);

  for (;;)
    {
      sysclock.sleep_for (1000);
    }
}

// ---------------------------------------------------------------------------
// µOS++ startup scaffolding (same pattern as smp_test0 / sd_test)
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

    static thread::stack::element_t main_stack[8192];
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
