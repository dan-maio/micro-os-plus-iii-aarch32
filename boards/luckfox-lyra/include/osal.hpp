/**
 * @file osal.hpp
 * @brief Thin OS abstraction facade for the variant-independent code.
 * @details Wraps µOS++ mutex and lock primitives.
 */

#pragma once

#include <cmsis-plus/rtos/os.h>

#ifndef E_SUCCESS
#define E_SUCCESS true
#endif

namespace osal {

/**
 * @class Mutex
 * @brief OS Abstraction Layer Mutex wrapping µOS++ mutex.
 */
class Mutex {
public:
    Mutex() : m_{"osal_mutex"} {}

    void wait() { m_.lock(); }
    void give() { m_.unlock(); }

    bool waitFor(unsigned int timeout_ticks) {
        return m_.timed_lock(timeout_ticks) == os::rtos::result::ok;
    }

private:
    os::rtos::mutex m_;
};

/**
 * @class LockGuard
 * @brief RAII scoped lock guard for osal::Mutex.
 */
class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_{m} { m_.wait(); }
    ~LockGuard() { m_.give(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_;
};

} // namespace osal
