/**
 * @file rk3506_stub.hpp
 * @brief Stub I/O functions for RK3506 bare-metal development.
 */

#ifndef RK3506_STUB_HPP_
#define RK3506_STUB_HPP_

#include <stdint.h>

namespace rk3506 {
namespace stub {

inline void writel(uint32_t val, uint32_t addr) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(addr);
    *ptr = val;
}

inline uint32_t readl(uint32_t addr) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(addr);
    return *ptr;
}

inline void writeb(uint8_t val, uint32_t addr) {
    volatile uint8_t* ptr = reinterpret_cast<volatile uint8_t*>(addr);
    *ptr = val;
}

inline uint8_t readb(uint32_t addr) {
    volatile uint8_t* ptr = reinterpret_cast<volatile uint8_t*>(addr);
    return *ptr;
}

}
}

extern "C" {

void increment_timer();
void init_gd();
void udelay(unsigned long us);
void mdelay(unsigned long msec);

}

#endif