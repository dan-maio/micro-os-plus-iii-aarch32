/**
 * @file bit_field.hpp
 * @brief Template-based bitfield and bitmask utilities for register manipulation.
 */

#ifndef BIT_FIELD_HPP_
#define BIT_FIELD_HPP_

#include <stdint.h>

namespace rk3506 {

/**
 * @class BitField
 * @brief Provides type-safe access to bitfields within a register.
 * 
 * Works by mapping a field defined by a mask and shift to a specific 
 * register memory address.
 * 
 * @tparam T The underlying integer type of the register (e.g. uint32_t).
 */
template <typename T>
class BitField {
public:
    /**
     * @brief Constructor.
     * @param addr Volatile pointer to the target register.
     */
    explicit BitField(volatile T* addr) : addr_(addr) {}

    /**
     * @brief Reads a specific bitfield.
     * @param mask Mask defining the bitfield (unshifted).
     * @param shift Number of bits to right-shift after masking.
     * @return The extracted field value.
     */
    T get_field(T mask, int shift) const {
        return (*addr_ & mask) >> shift;
    }

    /**
     * @brief Writes a specific bitfield.
     * @param mask Mask defining the bitfield (unshifted).
     * @param val Value to write to the field.
     * @param shift Number of bits to left-shift the value before masking/writing.
     */
    void set_field(T mask, T val, int shift) {
        T tmp = *addr_;
        tmp &= ~mask;
        tmp |= (val << shift) & mask;
        *addr_ = tmp;
    }

    /** @brief Sets all bits in the given mask. */
    void set_bits(T mask) {
        *addr_ |= mask;
    }

    /** @brief Clears all bits in the given mask. */
    void clear_bits(T mask) {
        *addr_ &= ~mask;
    }

    /** @brief Checks if any bits in the mask are set. */
    bool test_bits(T mask) const {
        return (*addr_ & mask) != 0;
    }

    /** @brief Reads the entire register value. */
    T read() const {
        return *addr_;
    }

    /** @brief Writes the entire register value. */
    void write(T val) {
        *addr_ = val;
    }

private:
    volatile T* addr_; /**< Memory-mapped address of the register. */
};

/**
 * @struct Bit
 * @brief Helper for single-bit flags.
 * @tparam POS Bit position (0-31).
 */
template<int POS>
struct Bit {
    static constexpr uint32_t MASK_VAL = (1U << POS); /**< Precalculated mask. */

    /** @brief Tests if the flag is set in the register. */
    template<typename R>
    static constexpr bool test(R reg) {
        return (reg & MASK_VAL) != 0;
    }

    /** @brief Returns register with the flag set. */
    template<typename R>
    static constexpr R set(R reg) {
        return reg | MASK_VAL;
    }

    /** @brief Returns register with the flag cleared. */
    template<typename R>
    static constexpr R clear(R reg) {
        return reg & ~MASK_VAL;
    }

    /** @brief Toggles the flag in the register value. */
    template<typename R>
    static constexpr R toggle(R reg) {
        return reg ^ MASK_VAL;
    }
};

}

#endif