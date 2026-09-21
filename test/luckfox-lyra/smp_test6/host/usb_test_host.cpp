/**
 * @file usb_test_host.cpp
 * @brief C++23 Host Application for testing RK3506 USB DWC2 Gadget device.
 */

#include "libusb_wrapper.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <expected>
#include <print>
#include <chrono>
#include <thread>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <format>
#include <charconv>

using namespace usb::host;

namespace {

constexpr uint16_t USB_VID = 0x2207; // Rockchip
constexpr uint16_t USB_PID = 0x0019; // Custom Interface PID
constexpr uint8_t EP_IN = 0x81;
constexpr uint8_t EP_OUT = 0x01;
constexpr unsigned int USB_TIMEOUT = 5000;
constexpr int MAX_RETRIES = 3;

uint8_t ep_in_addr = EP_IN;
uint8_t ep_out_addr = EP_OUT;

constexpr uint8_t CMD_ECHO = 0x01;
constexpr uint8_t CMD_BIG_TRANSFER = 0x02;
constexpr uint8_t CMD_BRIDGE = 0x03;   // usb0-1 firmware: forward to USB1/PC1

constexpr int BUFFER_SIZE = 512;
constexpr int BIG_HEADER_SIZE = 5;                  // [cmd, len_be32]
constexpr size_t BIG_URB_SIZE = 1048576;            // per-bulk_transfer window (libusb/kernel split into packets)
constexpr size_t BIG_MAX_BYTES = 256 * 1024 * 1024; // cap for generated patterns (held in RAM)
constexpr uint64_t BIG_FILE_MAX = 0xFFFFFFFFull;    // files are streamed from disk; 32-bit length field

/**
 * @brief Find and claim the specified USB device.
 */
std::expected<DeviceHandle, int> find_device(Context& ctx) {
    libusb_device** devs;
    auto cnt = libusb_get_device_list(ctx.get(), &devs);
    if (cnt < 0) return std::unexpected(static_cast<int>(cnt));

    libusb_device_handle* handle = nullptr;
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) == 0) {
            if (desc.idVendor == USB_VID && desc.idProduct == USB_PID) {
                if (libusb_open(devs[i], &handle) == 0) {
                    std::println("Found device: {:04x}:{:04x}", USB_VID, USB_PID);
                    break;
                }
            }
        }
    }
    libusb_free_device_list(devs, 1);

    if (!handle) return std::unexpected(LIBUSB_ERROR_NOT_FOUND);

    DeviceHandle dh(handle);
    libusb_set_auto_detach_kernel_driver(handle, 1);

    int cfg = 0;
    if (auto ret = libusb_get_configuration(handle, &cfg); ret == 0) {
        std::println("Current configuration: {}", cfg);
        if (cfg != 1) {
            auto sret = libusb_set_configuration(handle, 1);
            std::println("set_configuration(1): {} ({})", sret,
                         libusb_strerror(static_cast<libusb_error>(sret)));
        }
    } else {
        std::println(std::cerr, "get_configuration failed: {} ({})", ret,
                     libusb_strerror(static_cast<libusb_error>(ret)));
    }

    if (auto ret = dh.claim_interface(0); ret < 0) {
        std::println(std::cerr, "claim_interface(0) failed: {} ({})", ret,
                     libusb_strerror(static_cast<libusb_error>(ret)));
        return std::unexpected(ret);
    }

    std::println("Interface 0 claimed successfully");
    return dh;
}

/**
 * @brief Detect actual bulk endpoints from device configuration.
 */
int detect_bulk_endpoints(libusb_device_handle* handle) {
    libusb_device* dev = libusb_get_device(handle);
    libusb_config_descriptor* cfg = nullptr;
    int ret = libusb_get_active_config_descriptor(dev, &cfg);
    if (ret < 0 || !cfg || cfg->bNumInterfaces < 1) {
        std::println(std::cerr, "Warning: unable to read config, using defaults");
        return -1;
    }

    const auto* intf_desc = &cfg->interface[0].altsetting[0];
    bool in_found = false, out_found = false;
    for (int i = 0; i < intf_desc->bNumEndpoints; ++i) {
        const auto* ep = &intf_desc->endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;

        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
            ep_in_addr = ep->bEndpointAddress;
            in_found = true;
        } else {
            ep_out_addr = ep->bEndpointAddress;
            out_found = true;
        }
    }

    libusb_free_config_descriptor(cfg);

    if (!in_found || !out_found) {
        std::println(std::cerr, "Warning: endpoint discovery incomplete, using defaults IN={:#04x} OUT={:#04x}", ep_in_addr, ep_out_addr);
        return -1;
    }

    std::println("Using endpoints: IN={:#04x} OUT={:#04x}", ep_in_addr, ep_out_addr);
    return 0;
}

/**
 * @brief Write data to device with retries.
 */
std::expected<int, int> write_to_device(DeviceHandle& dev, std::span<const uint8_t> data, bool verbose = true) {
    int transferred = 0;
    int ret = 0;
    if (verbose)
        std::println("Writing {} bytes to EP_OUT ({:#04x})...", data.size(), ep_out_addr);
    for (int i = 0; i < MAX_RETRIES; ++i) {
        ret = dev.bulk_transfer(ep_out_addr, {const_cast<uint8_t*>(data.data()), data.size()}, transferred, USB_TIMEOUT);
        if (ret == 0) {
            if (verbose)
                std::println("Wrote {} bytes to device", transferred);
            return transferred;
        }
        
        std::println(std::cerr, "Error: write failed (attempt {}/{}): {} (ret={})", i + 1, MAX_RETRIES, error_name(ret), ret);
        if (ret == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(dev.get(), ep_out_addr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::unexpected(ret);
}

/**
 * @brief Read data from device with retries.
 */
std::expected<int, int> read_from_device(DeviceHandle& dev, std::span<uint8_t> data) {
    int transferred = 0;
    int ret = 0;
    for (int i = 0; i < MAX_RETRIES; ++i) {
        ret = dev.bulk_transfer(ep_in_addr, data, transferred, USB_TIMEOUT);
        if (ret == 0) return transferred;

        std::println(std::cerr, "Error: read failed (attempt {}/{}): {} (ret={})", i + 1, MAX_RETRIES, error_name(ret), ret);
        if (ret == LIBUSB_ERROR_PIPE) {
            libusb_clear_halt(dev.get(), ep_in_addr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::unexpected(ret);
}

/**
 * @brief Implementation of the loopback test.
 */
int do_loopback(DeviceHandle& dev, std::span<const uint8_t> test_data) {
    std::vector<uint8_t> send_buf;
    send_buf.reserve(test_data.size() + 1);
    send_buf.push_back(CMD_ECHO);
    send_buf.insert(send_buf.end(), test_data.begin(), test_data.end());

    std::println("Sending loopback data ({} bytes)...", send_buf.size());
    auto res = write_to_device(dev, send_buf);
    if (!res) return res.error();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<uint8_t> read_buf(BUFFER_SIZE);
    auto read_res = read_from_device(dev, read_buf);
    if (!read_res) return read_res.error();

    size_t received_len = *read_res;
    size_t data_start = (received_len > 0 && read_buf[0] == CMD_ECHO) ? 1 : 0;
    std::span<const uint8_t> received(read_buf.data() + data_start, received_len - data_start);

    bool match = (received.size() == test_data.size()) && std::memcmp(received.data(), test_data.data(), test_data.size()) == 0;
    if (match) {
        std::println("Loopback test PASSED");
        return 0;
    } else {
        std::println("Loopback test FAILED (Size: {} vs {})", received.size(), test_data.size());
        return -1;
    }
}

/**
 * @brief Implementation of the big transfer protocol.
 */
int do_big_transfer(DeviceHandle& dev, std::vector<uint8_t> data,
                    const std::filesystem::path& save_path = {}) {
    const size_t total_bytes = data.size();
    if (total_bytes == 0 || total_bytes > BIG_MAX_BYTES) {
        std::println(std::cerr, "Error: --big requires 1-{} bytes", BIG_MAX_BYTES);
        return -1;
    }

    // The device streams the echo back through a 64 KB FIFO while data is
    // still arriving, so reads must overlap writes: beyond the FIFO size a
    // write-everything-then-read approach deadlocks (device NAKs OUT with a
    // full ring until the echo is drained). A reader thread does the drain.
    std::vector<uint8_t> echo_buf(total_bytes);
    std::vector<uint8_t> ack(BIG_HEADER_SIZE + 2);
    std::atomic<int> reader_result{1};   // 1 = still running

    std::thread reader([&] {
        size_t echo_pos = 0;
        while (echo_pos < total_bytes) {
            size_t want = std::min(total_bytes - echo_pos, BIG_URB_SIZE);
            auto res = read_from_device(dev, std::span(echo_buf).subspan(echo_pos, want));
            if (!res) { reader_result = res.error(); return; }
            echo_pos += *res;
        }
        auto res = read_from_device(dev, ack);   // ACK: [0x02, len_be32, 'O', 'K']
        reader_result = res ? 0 : res.error();
    });

    // Protocol: header packet [0x02, len_be32, data...] filled to a full
    // 512-byte bulk packet, then raw 512-byte packets (last one partial).
    size_t pos = 0;
    int chunk_num = 0;
    int write_err = 0;
    while (pos < total_bytes || chunk_num == 0) {
        chunk_num++;
        std::vector<uint8_t> chunk_buf;
        if (chunk_num == 1) {
            // Header only, padded to a full packet (payload starts next packet)
            chunk_buf.assign(BUFFER_SIZE, 0);
            chunk_buf[0] = CMD_BIG_TRANSFER;
            chunk_buf[1] = static_cast<uint8_t>((total_bytes >> 24) & 0xFF);
            chunk_buf[2] = static_cast<uint8_t>((total_bytes >> 16) & 0xFF);
            chunk_buf[3] = static_cast<uint8_t>((total_bytes >> 8) & 0xFF);
            chunk_buf[4] = static_cast<uint8_t>(total_bytes & 0xFF);
        } else {
            size_t chunk_len = std::min(total_bytes - pos, BIG_URB_SIZE);
            chunk_buf.insert(chunk_buf.end(), data.begin() + pos, data.begin() + pos + chunk_len);
            pos += chunk_len;
        }

        if (auto res = write_to_device(dev, chunk_buf, false); !res) {
            write_err = res.error();
            break;
        }
    }

    if (write_err == 0)
        std::println("All {} bytes sent in {} chunks, draining echo...", total_bytes, chunk_num);

    reader.join();
    if (write_err != 0) return write_err;
    if (reader_result != 0) return reader_result;

    if (ack[0] == CMD_BIG_TRANSFER && ack[5] == 'O' && ack[6] == 'K') {
        uint32_t ack_count = (static_cast<uint32_t>(ack[1]) << 24) |
                             (static_cast<uint32_t>(ack[2]) << 16) |
                             (static_cast<uint32_t>(ack[3]) << 8)  |
                              static_cast<uint32_t>(ack[4]);
        std::println("ACK: count={}, status OK", ack_count);
    } else {
        std::println(std::cerr, "Error: Invalid ACK");
        return -1;
    }

    // Recombine the received stream on disk when the payload came from a file
    if (!save_path.empty()) {
        std::ofstream out(save_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::println(std::cerr, "Error: cannot create '{}'", save_path.string());
            return -1;
        }
        out.write(reinterpret_cast<const char*>(echo_buf.data()),
                  static_cast<std::streamsize>(total_bytes));
        if (!out) {
            std::println(std::cerr, "Error: short write to '{}'", save_path.string());
            return -1;
        }
        std::println("Saved {} echoed bytes to '{}'", total_bytes, save_path.string());
    }

    if (std::memcmp(data.data(), echo_buf.data(), total_bytes) == 0) {
        std::println("PASSED: all {} bytes match", total_bytes);
        return 0;
    } else {
        std::println(std::cerr, "FAILED: data mismatch");
        return -1;
    }
}

/**
 * @brief Streamed --big for files: reads the file from disk in windows,
 *        drains the echo concurrently (reader thread) into <file>.bak and
 *        verifies it against the original — constant memory, so files up
 *        to the protocol's 32-bit limit (4 GB - 1) work.
 */
int do_big_file_transfer(DeviceHandle& dev, const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t total_bytes = std::filesystem::file_size(path, ec);
    if (ec || total_bytes == 0 || total_bytes > BIG_FILE_MAX) {
        std::println(std::cerr, "Error: --big file must be 1-{} bytes", BIG_FILE_MAX);
        return -1;
    }
    std::filesystem::path bak = path;
    bak += ".bak";
    std::println("Streaming {} bytes from '{}' (echo -> '{}')...",
                 total_bytes, path.string(), bak.string());

    std::vector<uint8_t> ack(BIG_HEADER_SIZE + 2);
    std::atomic<int> reader_result{1};        // 1 = still running
    std::atomic<uint64_t> first_mismatch{UINT64_MAX};

    std::thread reader([&] {
        std::ifstream ref(path, std::ios::binary);
        std::ofstream out(bak, std::ios::binary | std::ios::trunc);
        if (!ref || !out) { reader_result = -1000; return; }
        std::vector<uint8_t> win(BIG_URB_SIZE), refwin(BIG_URB_SIZE);
        uint64_t pos = 0;
        while (pos < total_bytes) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(total_bytes - pos, BIG_URB_SIZE));
            auto res = read_from_device(dev, std::span(win).first(want));
            if (!res) { reader_result = res.error(); return; }
            size_t got = static_cast<size_t>(*res);
            out.write(reinterpret_cast<const char*>(win.data()),
                      static_cast<std::streamsize>(got));
            ref.read(reinterpret_cast<char*>(refwin.data()),
                     static_cast<std::streamsize>(got));
            if (first_mismatch == UINT64_MAX &&
                std::memcmp(win.data(), refwin.data(), got) != 0) {
                for (size_t i = 0; i < got; ++i)
                    if (win[i] != refwin[i]) { first_mismatch = pos + i; break; }
            }
            pos += got;
        }
        auto res = read_from_device(dev, ack);   // ACK: [0x02, len_be32, 'O', 'K']
        reader_result = res ? 0 : res.error();
    });

    std::ifstream f(path, std::ios::binary);
    int write_err = f ? 0 : -1000;
    auto t0 = std::chrono::steady_clock::now();

    // Header packet: [0x02, len_be32, pad] — one full bulk packet, NO
    // payload (payload here would misalign the device's zero-copy ring)
    if (write_err == 0) {
        std::vector<uint8_t> first(BUFFER_SIZE, 0);
        first[0] = CMD_BIG_TRANSFER;
        first[1] = static_cast<uint8_t>((total_bytes >> 24) & 0xFF);
        first[2] = static_cast<uint8_t>((total_bytes >> 16) & 0xFF);
        first[3] = static_cast<uint8_t>((total_bytes >> 8) & 0xFF);
        first[4] = static_cast<uint8_t>(total_bytes & 0xFF);
        if (auto res = write_to_device(dev, first, false); !res)
            write_err = res.error();

        std::vector<uint8_t> window(BIG_URB_SIZE);
        uint64_t sent = 0;
        while (write_err == 0 && sent < total_bytes) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(total_bytes - sent, BIG_URB_SIZE));
            f.read(reinterpret_cast<char*>(window.data()), static_cast<std::streamsize>(n));
            if (static_cast<size_t>(f.gcount()) != n) { write_err = -1001; break; }
            if (auto res = write_to_device(dev, std::span(window).first(n), false); !res)
                write_err = res.error();
            else
                sent += n;
        }
        if (write_err == 0)
            std::println("All {} bytes sent, draining echo...", total_bytes);
    }

    reader.join();
    if (write_err != 0) {
        std::println(std::cerr, "Error: send failed ({})", write_err);
        return write_err;
    }
    if (reader_result != 0) {
        std::println(std::cerr, "Error: echo drain failed ({})", reader_result.load());
        return reader_result;
    }

    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (secs > 0)
        std::println("Round-trip {:.2f} s  ({:.2f} MB/s each way)",
                     secs, static_cast<double>(total_bytes) / secs / 1e6);

    if (ack[0] == CMD_BIG_TRANSFER && ack[5] == 'O' && ack[6] == 'K') {
        uint32_t ack_count = (static_cast<uint32_t>(ack[1]) << 24) |
                             (static_cast<uint32_t>(ack[2]) << 16) |
                             (static_cast<uint32_t>(ack[3]) << 8)  |
                              static_cast<uint32_t>(ack[4]);
        std::println("ACK: count={}, status OK", ack_count);
    } else {
        std::println(std::cerr, "Error: Invalid ACK");
        return -1;
    }

    std::println("Saved {} echoed bytes to '{}'", total_bytes, bak.string());
    if (first_mismatch == UINT64_MAX) {
        std::println("PASSED: all {} bytes match", total_bytes);
        return 0;
    }
    std::println(std::cerr, "FAILED: first mismatch at offset {}", first_mismatch.load());
    return -1;
}

/**
 * @brief Send a file through the bridge: device forwards it out of USB1 to
 *        PC1 (run usb_recv_host there first). No echo comes back on USB0 —
 *        only the final 7-byte ACK after the last byte left USB1.
 */
int do_bridge_send(DeviceHandle& dev, const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t total_bytes = std::filesystem::file_size(path, ec);
    if (ec || total_bytes == 0 || total_bytes > BIG_FILE_MAX) {
        std::println(std::cerr, "Error: --bridge file must be 1-{} bytes", BIG_FILE_MAX);
        return -1;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::println(std::cerr, "Error: cannot open '{}'", path.string());
        return -1;
    }
    std::println("Bridging {} bytes from '{}' to PC1 via USB1...",
                 total_bytes, path.string());

    std::vector<uint8_t> first(BUFFER_SIZE, 0);
    first[0] = CMD_BRIDGE;
    first[1] = static_cast<uint8_t>((total_bytes >> 24) & 0xFF);
    first[2] = static_cast<uint8_t>((total_bytes >> 16) & 0xFF);
    first[3] = static_cast<uint8_t>((total_bytes >> 8) & 0xFF);
    first[4] = static_cast<uint8_t>(total_bytes & 0xFF);
    if (auto res = write_to_device(dev, first, false); !res) return res.error();

    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> window(BIG_URB_SIZE);
    uint64_t sent = 0;
    while (sent < total_bytes) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(total_bytes - sent, BIG_URB_SIZE));
        f.read(reinterpret_cast<char*>(window.data()), static_cast<std::streamsize>(n));
        if (static_cast<size_t>(f.gcount()) != n) {
            std::println(std::cerr, "Error: short read on '{}'", path.string());
            return -1;
        }
        if (auto res = write_to_device(dev, std::span(window).first(n), false); !res)
            return res.error();
        sent += n;
    }
    std::println("All {} bytes sent, waiting for forward ACK...", total_bytes);

    // ACK arrives only after USB1 delivered the last byte to PC1 — allow
    // for PC1 lag by retrying timeouts here.
    std::vector<uint8_t> ack(BIG_HEADER_SIZE + 2);
    int got = 0, ret = 0;
    for (int i = 0; i < 30; ++i) {
        ret = dev.bulk_transfer(ep_in_addr, ack, got, USB_TIMEOUT);
        if (ret != LIBUSB_ERROR_TIMEOUT) break;
    }
    if (ret != 0) {
        std::println(std::cerr, "Error: no ACK ({})", error_name(ret));
        return ret;
    }
    if (!(ack[0] == CMD_BIG_TRANSFER && ack[5] == 'O' && ack[6] == 'K')) {
        std::println(std::cerr, "Error: Invalid ACK");
        return -1;
    }
    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::println("Bridged {} bytes in {:.2f} s ({:.2f} MB/s); verify on PC1",
                 total_bytes, secs,
                 secs > 0 ? static_cast<double>(total_bytes) / secs / 1e6 : 0.0);
    return 0;
}

/**
 * @brief Send a file into the USB0->SD->USB1 pipeline firmware. Identical
 *        wire framing to --bridge (CMD_BRIDGE + len, then payload, then a
 *        final ACK), but the OUT stream is SD-paced: the device NAK-flow-
 *        controls USB0 down to ~SD speed once its buffers fill, so we send
 *        small (64 KB) chunks and advance by the actually-transferred count
 *        on a timeout (libusb reports partial progress), under a generous
 *        size-based deadline. PC1 runs usb_recv_host as for --bridge.
 */
int do_pipe_send(DeviceHandle& dev, const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t total_bytes = std::filesystem::file_size(path, ec);
    if (ec || total_bytes == 0 || total_bytes > BIG_FILE_MAX) {
        std::println(std::cerr, "Error: --pipe file must be 1-{} bytes", BIG_FILE_MAX);
        return -1;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::println(std::cerr, "Error: cannot open '{}'", path.string()); return -1; }
    // Route-neutral wording: --pipe just streams into USB0; what happens next
    // (capture to SD, relay to PC1, pipeline) is the RESIDENT IMAGE's route.
    std::println("Piping {} bytes from '{}' into USB0...",
                 total_bytes, path.string());

    // deadline: assume >= 30 KB/s sustained, plus 120 s slack
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    auto deadline = t0 + seconds(static_cast<long>(total_bytes / (30 * 1024) + 120));

    // command packet: [CMD_BRIDGE, len_be32, pad] in one 512-byte OUT
    std::vector<uint8_t> first(BUFFER_SIZE, 0);
    first[0] = CMD_BRIDGE;
    first[1] = static_cast<uint8_t>((total_bytes >> 24) & 0xFF);
    first[2] = static_cast<uint8_t>((total_bytes >> 16) & 0xFF);
    first[3] = static_cast<uint8_t>((total_bytes >> 8) & 0xFF);
    first[4] = static_cast<uint8_t>(total_bytes & 0xFF);
    if (auto res = write_to_device(dev, first, false); !res) return res.error();

    constexpr size_t PIPE_CHUNK = 65536;
    std::vector<uint8_t> window(PIPE_CHUNK);
    uint64_t sent = 0;
    while (sent < total_bytes) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(total_bytes - sent, PIPE_CHUNK));
        f.read(reinterpret_cast<char*>(window.data()), static_cast<std::streamsize>(n));
        if (static_cast<size_t>(f.gcount()) != n) {
            std::println(std::cerr, "Error: short read on '{}'", path.string());
            return -1;
        }
        size_t off = 0;
        while (off < n) {
            int tr = 0;
            int ret = dev.bulk_transfer(ep_out_addr,
                          {window.data() + off, n - off}, tr, USB_TIMEOUT);
            off += static_cast<size_t>(tr);        // partial progress counts
            if (ret == 0) continue;
            if (ret == LIBUSB_ERROR_TIMEOUT) {
                if (steady_clock::now() > deadline) {
                    std::println(std::cerr, "Error: OUT stalled past deadline at {}", sent + off);
                    return ret;
                }
                continue;                          // SD-paced NAK: keep going
            }
            if (ret == LIBUSB_ERROR_PIPE) libusb_clear_halt(dev.get(), ep_out_addr);
            std::println(std::cerr, "Error: OUT failed at {}: {}", sent + off, error_name(ret));
            return ret;
        }
        sent += n;
    }
    std::println("All {} bytes sent, waiting for pipeline ACK...", total_bytes);

    std::vector<uint8_t> ack(BIG_HEADER_SIZE + 2);
    int got = 0, ret = 0;
    while (steady_clock::now() < deadline) {
        ret = dev.bulk_transfer(ep_in_addr, ack, got, USB_TIMEOUT);
        if (ret != LIBUSB_ERROR_TIMEOUT) break;
    }
    if (ret != 0) { std::println(std::cerr, "Error: no ACK ({})", error_name(ret)); return ret; }
    if (!(ack[0] == CMD_BIG_TRANSFER && ack[5] == 'O' && ack[6] == 'K')) {
        std::println(std::cerr, "Error: Invalid ACK"); return -1;
    }
    auto s = duration<double>(steady_clock::now() - t0).count();
    std::println("Piped {} bytes in {:.1f} s ({:.1f} KB/s); device ACKed completion",
                 total_bytes, s, s > 0 ? total_bytes / s / 1024.0 : 0.0);
    return 0;
}

/**
 * @brief Build the --big payload for numeric arguments: a generated
 *        pattern of <arg> bytes (files take the streaming path instead).
 */
std::expected<std::vector<uint8_t>, std::string> load_big_payload(std::string_view arg) {
    int n = 0;
    auto [ptr, err] = std::from_chars(arg.data(), arg.data() + arg.size(), n);
    if (err != std::errc{} || ptr != arg.data() + arg.size())
        return std::unexpected(std::format("'{}' is neither a file nor a byte count", arg));
    if (n <= 0 || n > static_cast<int>(BIG_MAX_BYTES))
        return std::unexpected(std::format("byte count must be 1-{}", BIG_MAX_BYTES));
    std::vector<uint8_t> data(n);
    for (int i = 0; i < n; ++i) {
        data[i] = static_cast<uint8_t>((i % 2 == 0) ? ('A' + (i / 2) % 26) : ('a' + (i / 2) % 26));
    }
    return data;
}

} // namespace

/// Host entry: dispatch write/read/loopback/big/bridge/pipe against USB0 (2207:0019). @param argc Arg count. @param argv Args. @return 0 on success.
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println("Usage: {} [options]\nOptions:\n  --write <text>\n  --read [bytes]\n  --loopback\n  --big <file|bytes> (files streamed from disk, max 4GB, echo saved as <file>.bak; patterns max 256MB)\n  --bridge <file> (usb0-1 firmware: forward the file out of USB1 to PC1's usb_recv_host)", argv[0]);
        return 1;
    }

    auto ctx_res = Context::create();
    if (!ctx_res) {
        std::println(std::cerr, "Failed to init libusb: {}", ctx_res.error());
        return 1;
    }
    Context& ctx = *ctx_res;

    auto dev_res = find_device(ctx);
    if (!dev_res) {
        std::println(std::cerr, "Failed to find/claim device: {}", dev_res.error());
        return 1;
    }
    DeviceHandle& dev = *dev_res;

    detect_bulk_endpoints(dev.get());

    std::string_view cmd = argv[1];
    if (cmd == "--write" && argc >= 3) {
        std::string_view text = argv[2];
        std::vector<uint8_t> send_buf;
        send_buf.reserve(text.size() + 1);
        send_buf.push_back(CMD_ECHO);
        for (char c : text) send_buf.push_back(static_cast<uint8_t>(c));
        
        auto res = write_to_device(dev, send_buf);
        if (!res) return 1;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::vector<uint8_t> read_buf(BUFFER_SIZE);
        auto read_res = read_from_device(dev, read_buf);
        if (read_res && *read_res > 0) {
            size_t data_start = (*read_res > 0 && read_buf[0] == CMD_ECHO) ? 1 : 0;
            std::string_view received(reinterpret_cast<const char*>(read_buf.data() + data_start), *read_res - data_start);
            std::println("Echo response: {}", received);
            std::println("Write OK");
        }
    } else if (cmd == "--read") {
        int bytes = (argc >= 3) ? std::stoi(argv[2]) : 64;
        std::vector<uint8_t> buf(bytes);
        auto res = read_from_device(dev, buf);
        if (res) {
            std::string_view data(reinterpret_cast<const char*>(buf.data()), *res);
            std::println("Data: {}", data);
        } else return 1;
    } else if (cmd == "--loopback") {
        std::string_view payload = "Hello RK3506 C++23 Loopback!";
        std::vector<uint8_t> test_payload(payload.begin(), payload.end());
        return do_loopback(dev, test_payload);
    } else if (cmd == "--bridge" && argc >= 3) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(argv[2], ec)) {
            std::println(std::cerr, "Error: '{}' is not a file", argv[2]);
            return 1;
        }
        return do_bridge_send(dev, argv[2]);
    } else if (cmd == "--pipe" && argc >= 3) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(argv[2], ec)) {
            std::println(std::cerr, "Error: '{}' is not a file", argv[2]);
            return 1;
        }
        return do_pipe_send(dev, argv[2]);
    } else if (cmd == "--big" && argc >= 3) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(argv[2], ec)) {
            // Files stream from disk (constant RAM, up to 4 GB - 1)
            return do_big_file_transfer(dev, argv[2]);
        }
        auto payload = load_big_payload(argv[2]);
        if (!payload) {
            std::println(std::cerr, "Error: {}", payload.error());
            return 1;
        }
        return do_big_transfer(dev, std::move(*payload));
    } else {
        std::println(std::cerr, "Unknown or invalid command");
        return 1;
    }

    return 0;
}
