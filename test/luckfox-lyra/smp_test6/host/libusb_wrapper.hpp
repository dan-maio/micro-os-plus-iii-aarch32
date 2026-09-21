/**
 * @file libusb_wrapper.hpp
 * @brief Modern C++23 RAII wrapper for libusb-1.0 on the host PC.
 */

#ifndef LIBUSB_WRAPPER_HPP_
#define LIBUSB_WRAPPER_HPP_

#include <libusb-1.0/libusb.h>
#include <memory>
#include <expected>
#include <string>
#include <vector>
#include <span>
#include <utility>

namespace usb::host {

/**
 * @enum Error
 * @brief USB-specific error codes mapped from libusb.
 */
enum class Error {
    IO = LIBUSB_ERROR_IO,
    InvalidParam = LIBUSB_ERROR_INVALID_PARAM,
    Access = LIBUSB_ERROR_ACCESS,
    NoDevice = LIBUSB_ERROR_NO_DEVICE,
    NotFound = LIBUSB_ERROR_NOT_FOUND,
    Busy = LIBUSB_ERROR_BUSY,
    Timeout = LIBUSB_ERROR_TIMEOUT,
    Overflow = LIBUSB_ERROR_OVERFLOW,
    Pipe = LIBUSB_ERROR_PIPE,
    Interrupted = LIBUSB_ERROR_INTERRUPTED,
    NoMem = LIBUSB_ERROR_NO_MEM,
    NotSupported = LIBUSB_ERROR_NOT_SUPPORTED,
    Other = LIBUSB_ERROR_OTHER,
};

/**
 * @brief Convert a libusb error code to a human-readable string.
 * @param code A libusb error code.
 * @return The corresponding error name.
 */
inline std::string error_name(int code) {
    return libusb_error_name(code);
}

/**
 * @class Context
 * @brief RAII wrapper for libusb_context.
 */
class Context {
public:
    /// Create a libusb context. @return The Context, or a libusb error code on failure.
    static std::expected<Context, int> create() {
        libusb_context* ctx = nullptr;
        int ret = libusb_init(&ctx);
        if (ret < 0) return std::unexpected(ret);
        return Context(ctx);
    }

    ~Context() {
        if (ctx_) libusb_exit(ctx_);
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    /// Move constructor: transfer ownership of the context. @param other Source (left empty).
    Context(Context&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}
    /// Move assignment: transfer ownership of the context. @param other Source (left empty). @return *this.
    Context& operator=(Context&& other) noexcept {
        if (this != &other) {
            if (ctx_) libusb_exit(ctx_);
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    /// @return The underlying libusb_context pointer.
    libusb_context* get() const { return ctx_; }

private:
    explicit Context(libusb_context* ctx) : ctx_(ctx) {}
    libusb_context* ctx_ = nullptr;
};

/**
 * @class DeviceHandle
 * @brief RAII wrapper for libusb_device_handle.
 */
class DeviceHandle {
public:
    /// Adopt an open device handle. @param handle An open libusb_device_handle (ownership taken).
    DeviceHandle(libusb_device_handle* handle) : handle_(handle) {}

    ~DeviceHandle() {
        if (handle_) {
            if (interface_claimed_) {
                libusb_release_interface(handle_, 0);
            }
            libusb_close(handle_);
        }
    }

    DeviceHandle(const DeviceHandle&) = delete;
    DeviceHandle& operator=(const DeviceHandle&) = delete;
    /// Move constructor: transfer the handle and claim state. @param other Source (left empty).
    DeviceHandle(DeviceHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          interface_claimed_(std::exchange(other.interface_claimed_, false)) {}

    /// Move assignment: transfer the handle and claim state. @param other Source (left empty). @return *this.
    DeviceHandle& operator=(DeviceHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                if (interface_claimed_) libusb_release_interface(handle_, 0);
                libusb_close(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
            interface_claimed_ = std::exchange(other.interface_claimed_, false);
        }
        return *this;
    }

    /// Claim an interface. @param interface Interface number. @return 0 on success, else a libusb error code.
    int claim_interface(int interface) {
        int ret = libusb_claim_interface(handle_, interface);
        if (ret == 0) interface_claimed_ = true;
        return ret;
    }

    /**
     * @brief Perform a bulk transfer.
     * @param endpoint    Endpoint address (direction bit included).
     * @param data        Buffer to send or receive into.
     * @param transferred Out: number of bytes actually transferred.
     * @param timeout     Timeout in milliseconds (0 = unlimited).
     * @return 0 on success, else a libusb error code.
     */
    int bulk_transfer(uint8_t endpoint, std::span<uint8_t> data, int& transferred, unsigned int timeout) {
        return libusb_bulk_transfer(handle_, endpoint, data.data(), static_cast<int>(data.size()), &transferred, timeout);
    }

    /// @return The underlying libusb_device_handle pointer.
    libusb_device_handle* get() const { return handle_; }

private:
    libusb_device_handle* handle_ = nullptr;
    bool interface_claimed_ = false;
};

} // namespace usb::host

#endif // LIBUSB_WRAPPER_HPP_
