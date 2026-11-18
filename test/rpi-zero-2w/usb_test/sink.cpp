/*
 * sink.cpp — storage backends for usb_test.
 */
#include <cstddef>
#include <cstdint>

#include "protocol.hpp"
#include "sink.hpp"

#if defined(HW_BUILD)
#include "fatfs_hw.hpp"
#endif

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
      if (i >= flatfs::kNameMax)
        {
          return false;
        }
    }
  return true;
}

Sink::Result
map_flatfs (flatfs::FlatFs::Result r) noexcept
{
  switch (r)
    {
    case flatfs::FlatFs::Result::ok:
      return Sink::Result::ok;
    case flatfs::FlatFs::Result::err_not_found:
      return Sink::Result::not_found;
    case flatfs::FlatFs::Result::err_no_space:
    case flatfs::FlatFs::Result::err_full:
      return Sink::Result::no_space;
    case flatfs::FlatFs::Result::err_name:
      return Sink::Result::bad_name;
    case flatfs::FlatFs::Result::err_buf:
      return Sink::Result::too_large;
    default:
      return Sink::Result::io_error;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FlatFsSink
// ---------------------------------------------------------------------------
Sink::Result
FlatFsSink::put (const char* n, const void* data, std::uint32_t len)
{
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  return map_flatfs (fs_.write_file (n, data, len));
}

Sink::Result
FlatFsSink::get (const char* n, void* buf, std::uint32_t cap,
                 std::uint32_t& out_len)
{
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  out_len = 0u;
  return map_flatfs (fs_.read_file (n, buf, cap, &out_len));
}

Sink::Result
FlatFsSink::list (flatfs::FileVisitor v, void* ctx)
{
  return map_flatfs (fs_.list (v, ctx));
}

// ---------------------------------------------------------------------------
// RamUartSink
// ---------------------------------------------------------------------------
#if defined(HW_BUILD)

namespace
{

// One character of an 8.3 name. FatFs with FF_USE_LFN = 0 accepts upper-case
// alphanumerics and a small punctuation set; everything else becomes '_'.
char
up83 (char c) noexcept
{
  if (c >= 'a' && c <= 'z')
    {
      return static_cast<char> (c - 'a' + 'A');
    }
  const bool keep = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '_' || c == '-' || c == '~';
  return keep ? c : '_';
}

struct FatVisitPack
{
  flatfs::FileVisitor v;
  void* ctx;
};

bool
fat_visit_trampoline (const char* n, std::uint32_t size, void* ctx) noexcept
{
  FatVisitPack* p = static_cast<FatVisitPack*> (ctx);
  flatfs::FileEntry e;
  std::uint32_t i = 0;
  for (; n[i] != '\0' && i < flatfs::kNameMax; ++i)
    {
      e.name[i] = n[i];
    }
  e.name[i] = '\0';
  e.start_block = 0u;
  e.size_bytes = size;
  e.valid = true;
  return p->v (e, p->ctx);
}

} // anonymous namespace

void
FatFsSink::to_83 (const char* n, char out[13]) noexcept
{
  std::size_t len = 0;
  while (n[len] != '\0')
    {
      ++len;
    }
  // Split at the LAST dot, so "a.tar.gz" keeps "GZ".
  std::size_t stem_len = len;
  for (std::size_t i = 0; i < len; ++i)
    {
      if (n[i] == '.')
        {
          stem_len = i;
        }
    }

  char stem[9];
  std::size_t sn = 0;
  for (std::size_t i = 0; i < stem_len && sn < 8u; ++i)
    {
      stem[sn++] = up83 (n[i]);
    }
  if (stem_len > 8u)
    {
      // Truncated. The classic "~1" tail keeps names that share their first
      // six characters from colliding on the leading eight.
      stem[6] = '~';
      stem[7] = '1';
      sn = 8u;
    }
  if (sn == 0u)
    {
      stem[sn++] = '_';
    }

  char ext[4];
  std::size_t en = 0;
  if (stem_len < len)
    {
      for (std::size_t i = stem_len + 1u; i < len && en < 3u; ++i)
        {
          ext[en++] = up83 (n[i]);
        }
    }

  std::size_t o = 0;
  for (std::size_t i = 0; i < sn; ++i)
    {
      out[o++] = stem[i];
    }
  if (en != 0u)
    {
      out[o++] = '.';
      for (std::size_t i = 0; i < en; ++i)
        {
          out[o++] = ext[i];
        }
    }
  out[o] = '\0';
}

bool
FatFsSink::mount () noexcept
{
  return fatfshw::mount_volume () && fatfshw::ensure_tests_dir ();
}

Sink::Result
FatFsSink::put (const char* n, const void* data, std::uint32_t len)
{
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  char leaf[13];
  to_83 (n, leaf);
  return fatfshw::store_file (leaf, data, len) ? Result::ok : Result::io_error;
}

Sink::Result
FatFsSink::get (const char* n, void* buf, std::uint32_t cap,
                std::uint32_t& out_len)
{
  out_len = 0u;
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  char leaf[13];
  to_83 (n, leaf);
  std::uint32_t size = 0u;
  if (!fatfshw::file_size (leaf, size))
    {
      return Result::not_found;
    }
  if (size > cap)
    {
      return Result::too_large;
    }
  if (!fatfshw::load_file (leaf, buf, size))
    {
      return Result::io_error;
    }
  out_len = size;
  return Result::ok;
}

Sink::Result
FatFsSink::list (flatfs::FileVisitor v, void* ctx)
{
  FatVisitPack p{ v, ctx };
  (void)fatfshw::visit_tests_dir (&fat_visit_trampoline, &p);
  return Result::ok;
}

#endif // HW_BUILD

std::uint8_t RamUartSink::buf_[kRamFileMax] = {};

Sink::Result
RamUartSink::put (const char* n, const void* data, std::uint32_t len)
{
  if (!valid_name (n))
    {
      return Result::bad_name;
    }
  std::uint32_t i = 0;
  for (; n[i] != '\0' && i < flatfs::kNameMax; ++i)
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
RamUartSink::list (flatfs::FileVisitor v, void* ctx)
{
  if (!has_)
    {
      return Result::ok; // empty listing
    }
  flatfs::FileEntry e;
  std::uint32_t i = 0;
  for (; name_[i] != '\0' && i < flatfs::kNameMax; ++i)
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
