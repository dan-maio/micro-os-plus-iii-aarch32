/*
 * sink.cpp — storage backends for usb_test.
 */
#include <cmsis-plus/posix-io/directory.h>
#include <cmsis-plus/posix-io/file-system.h>
#include <cmsis-plus/posix-io/io.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "protocol.hpp"
#include "sink.hpp"

namespace usbtest
{

std::uint32_t
crc32 (const void* data, std::uint32_t len) noexcept
{
  const std::uint8_t* p = static_cast<const std::uint8_t*> (data);
  std::uint32_t crc = 0xFFFF'FFFFu;
  for (std::uint32_t i = 0; i < len; ++i)
    {
      crc ^= p[i];
      for (int k = 0; k < 8; ++k)
        {
          crc = (crc >> 1) ^ (0xEDB8'8320u & (0u - (crc & 1u)));
        }
    }
  return ~crc;
}

namespace
{

bool
valid_name (const char* n) noexcept
{
  if (n == nullptr || n[0] == '\0')
    {
      return false;
    }
  std::size_t i = 0;
  for (; n[i] != '\0'; ++i)
    {
      if (i >= kNameMax)
        {
          return false;
        }
      // A name, not a path: it must stay in /sd/tests/.
      if (n[i] == '/' || n[i] == '\\')
        {
          return false;
        }
    }
  return true;
}

// The file's path on the card.
void
card_path (const char* n, char out[48]) noexcept
{
  std::snprintf (out, 48, "/sd/tests/%s", n);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PosixSink
// ---------------------------------------------------------------------------
Sink::Result
PosixSink::put (const char* n, const void* data, std::uint32_t len)
{
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  char path[48];
  card_path (n, path);
  os::posix::io* f = os::posix::open (path, O_WRONLY | O_CREAT | O_TRUNC);
  if (f == nullptr)
    {
      return (errno == ENOSPC) ? Result::no_space : Result::io_error;
    }
  const bool ok = f->write (data, len) == static_cast<ssize_t> (len);
  const int err = errno;
  if (f->close () != 0 || !ok)
    {
      return (err == ENOSPC) ? Result::no_space : Result::io_error;
    }
  return Result::ok;
}

Sink::Result
PosixSink::get (const char* n, void* buf, std::uint32_t cap,
                std::uint32_t& out_len)
{
  out_len = 0u;
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  char path[48];
  card_path (n, path);
  struct stat st;
  if (os::posix::stat (path, &st) != 0)
    {
      return Result::not_found;
    }
  const std::uint32_t size = static_cast<std::uint32_t> (st.st_size);
  if (size > cap)
    {
      return Result::too_large;
    }
  os::posix::io* f = os::posix::open (path, O_RDONLY);
  if (f == nullptr)
    {
      return Result::io_error;
    }
  const bool ok = f->read (buf, size) == static_cast<ssize_t> (size);
  f->close ();
  if (!ok)
    {
      return Result::io_error;
    }
  out_len = size;
  return Result::ok;
}

Sink::Result
PosixSink::list (FileVisitor v, void* ctx)
{
  os::posix::directory* dir = os::posix::opendir ("/sd/tests");
  if (dir == nullptr)
    {
      return Result::io_error;
    }
  for (dirent* d = dir->read (); d != nullptr; d = dir->read ())
    {
      FileEntry e;
      std::uint32_t i = 0;
      for (; d->d_name[i] != '\0' && i < kNameMax; ++i)
        {
          e.name[i] = d->d_name[i];
        }
      e.name[i] = '\0';
      char path[48];
      card_path (d->d_name, path);
      struct stat st;
      e.size_bytes = (os::posix::stat (path, &st) == 0)
                         ? static_cast<std::uint32_t> (st.st_size)
                         : 0u;
      e.start_block = 0u;
      e.valid = true;
      if (!v (e, ctx))
        {
          break;
        }
    }
  dir->close ();
  return Result::ok;
}

std::uint8_t RamUartSink::buf_[kRamFileMax] = {};

Sink::Result
RamUartSink::put (const char* n, const void* data, std::uint32_t len)
{
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  std::uint32_t i = 0;
  for (; n[i] != '\0' && i < kNameMax; ++i)
    {
      name_[i] = n[i];
    }
  name_[i] = '\0';

  len_ = (len < kRamFileMax) ? len : kRamFileMax;
  truncated_ = (len > kRamFileMax);
  const std::uint8_t* p = static_cast<const std::uint8_t*> (data);
  for (std::uint32_t k = 0; k < len_; ++k)
    {
      buf_[k] = p[k];
    }
  has_ = true;
  return Result::ok;
}

Sink::Result
RamUartSink::get (const char* n, void* buf, std::uint32_t cap,
                  std::uint32_t& out_len)
{
  out_len = 0u;
  if (!has_)
    {
      return Result::not_found;
    }
  if (n == nullptr || n[0] == '\0')
    {
      return Result::bad_name;
    }
  std::uint32_t i = 0;
  for (; n[i] != '\0' && name_[i] != '\0'; ++i)
    {
      if (n[i] != name_[i])
        {
          return Result::not_found;
        }
    }
  if (n[i] != '\0' || name_[i] != '\0')
    {
      return Result::not_found;
    }
  if (truncated_)
    {
      return Result::too_large;
    }
  if (len_ > cap)
    {
      return Result::too_large;
    }
  std::uint8_t* d = static_cast<std::uint8_t*> (buf);
  for (std::uint32_t k = 0; k < len_; ++k)
    {
      d[k] = buf_[k];
    }
  out_len = len_;
  return Result::ok;
}

Sink::Result
RamUartSink::list (FileVisitor v, void* ctx)
{
  if (!has_)
    {
      return Result::ok; // empty listing
    }
  FileEntry e;
  std::uint32_t i = 0;
  for (; name_[i] != '\0' && i < kNameMax; ++i)
    {
      e.name[i] = name_[i];
    }
  e.name[i] = '\0';
  e.start_block = 0u;
  e.size_bytes = len_;
  e.valid = true;
  v (e, ctx);
  return Result::ok;
}

} // namespace usbtest
