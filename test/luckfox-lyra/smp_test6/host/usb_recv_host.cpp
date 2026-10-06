/**
 * @file usb_recv_host.cpp
 * @brief PC1 receiver for the USB0->USB1 file bridge (make pc1).
 *
 * The Lyra's bridge firmware (make usb0-1) presents its OTG1 port as a
 * device (2207:001A) with a single bulk IN endpoint. The device first
 * sends a 512-byte header [0x02, len_be32, pad], then streams the file.
 * This tool reads the stream and saves it to disk.
 *
 * Usage: usb_recv_host <output-file>
 */

#include "libusb_wrapper.hpp"
#include <iostream>
#include <vector>
#include <span>
#include <print>
#include <fstream>
#include <chrono>
#include <cstring>

using namespace usb::host;

namespace {

constexpr uint16_t USB_VID = 0x2207;
constexpr uint16_t USB_PID = 0x001A;          // bridge output port
constexpr uint8_t  EP_IN   = 0x81;
constexpr unsigned int USB_TIMEOUT = 10000;   // ms; bridge waits on PC0
constexpr uint8_t  CMD_BIG_TRANSFER = 0x02;
constexpr size_t   URB_SIZE = 1048576;

} // namespace

/// Host entry: receive a bridged/piped stream on USB1 (2207:001A) to a file. @param argc Arg count. @param argv Args. @return 0 on success.
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println("Usage: {} <output-file>", argv[0]);
        return 1;
    }

    auto ctx_res = Context::create();
    if (!ctx_res) {
        std::println(std::cerr, "Failed to init libusb: {}", ctx_res.error());
        return 1;
    }

    libusb_device_handle* handle =
        libusb_open_device_with_vid_pid(ctx_res->get(), USB_VID, USB_PID);
    if (!handle) {
        std::println(std::cerr, "Bridge device {:04x}:{:04x} not found "
                     "(is the Lyra running the usb0-1 firmware?)", USB_VID, USB_PID);
        return 1;
    }
    DeviceHandle dev(handle);
    libusb_set_auto_detach_kernel_driver(handle, 1);
    if (int cfg = 0; libusb_get_configuration(handle, &cfg) == 0 && cfg != 1)
        libusb_set_configuration(handle, 1);
    if (auto ret = dev.claim_interface(0); ret < 0) {
        std::println(std::cerr, "claim_interface failed: {}", ret);
        return 1;
    }
    std::println("Bridge device {:04x}:{:04x} ready, waiting for header...",
                 USB_VID, USB_PID);

    // Header: [0x02, len_be32, pad] — one 512-byte packet. Poll until the
    // sender on PC0 actually starts (timeouts here are idle, not fatal).
    std::vector<uint8_t> hdr(512);
    uint64_t total = 0;
    while (true) {
        int got = 0;
        int ret = dev.bulk_transfer(EP_IN, hdr, got, USB_TIMEOUT);
        if (ret == LIBUSB_ERROR_TIMEOUT) {
            std::print(".");
            std::fflush(stdout);
            continue;
        }
        if (ret != 0) {
            std::println(std::cerr, "\nheader read failed: {}", error_name(ret));
            return 1;
        }
        if (got >= 5 && hdr[0] == CMD_BIG_TRANSFER) {
            total = (static_cast<uint64_t>(hdr[1]) << 24) |
                    (static_cast<uint64_t>(hdr[2]) << 16) |
                    (static_cast<uint64_t>(hdr[3]) << 8)  |
                     static_cast<uint64_t>(hdr[4]);
            break;
        }
        std::println(std::cerr, "\nunexpected packet ({} bytes), ignoring", got);
    }
    std::println("\nIncoming file: {} bytes -> '{}'", total, argv[1]);

    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    if (!out) {
        std::println(std::cerr, "cannot create '{}'", argv[1]);
        return 1;
    }

    std::vector<uint8_t> win(URB_SIZE);
    uint64_t pos = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (pos < total) {
        // Request only 512-byte multiples (safe for FS mps=64 and HS
        // mps=512): a request ending mid-packet makes the kernel report
        // EOVERFLOW when the final full packet doesn't fit. The device
        // terminates with a short packet (or ZLP) so the URB completes
        // at the right size anyway.
        uint64_t remaining = total - pos;
        size_t want = static_cast<size_t>(std::min<uint64_t>(
            (remaining + 511) & ~511ull, URB_SIZE));
        int got = 0;
        int ret = dev.bulk_transfer(EP_IN, std::span(win).first(want), got, USB_TIMEOUT);
        if (ret != 0 && ret != LIBUSB_ERROR_TIMEOUT) {
            std::println(std::cerr, "read failed at {}: {}", pos, error_name(ret));
            return 1;
        }
        if (got > 0) {
            out.write(reinterpret_cast<const char*>(win.data()), got);
            pos += static_cast<uint64_t>(got);
        }
    }
    out.close();

    // Termination drain: when the total is a 512-multiple our final URB
    // completed on buffer-full, but the device still sends a terminating
    // ZLP (needed for the other rounding cases). Consume it — otherwise
    // it stays queued on the device's IN endpoint, its completion never
    // fires, and the sender on PC0 never gets the forward ACK.
    if ((total % 512ull) == 0) {
        int got = 0;
        std::vector<uint8_t> drain(512);
        (void)dev.bulk_transfer(EP_IN, drain, got, 500);
    }

    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::println("Received {} bytes in {:.2f} s ({:.2f} MB/s), saved to '{}'",
                 total, secs, secs > 0 ? static_cast<double>(total) / secs / 1e6 : 0.0,
                 argv[1]);
    return 0;
}
