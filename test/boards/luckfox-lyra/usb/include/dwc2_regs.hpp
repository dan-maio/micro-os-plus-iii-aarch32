/**
 * @file dwc2_regs.hpp
 * @brief Strongly-typed C++23 register map and bit field definitions for the DWC2 USB Controller.
 */

#ifndef DWC2_REGS_HPP_
#define DWC2_REGS_HPP_

#include <stdint.h>
#include <stddef.h>

namespace rk3506::usb::dwc2 {

/** @brief Logical physical base address of USB0 DWC2 controller. */
constexpr uintptr_t USB_OTG0_BASE = 0xFF740000U;
/** @brief Logical physical base address of USB1 DWC2 controller. */
constexpr uintptr_t USB_OTG1_BASE = 0xFF780000U;

/** @brief Clock and Reset Unit Base. */
constexpr uintptr_t CRU_BASE = 0xFF9A0000U;
/** @brief General Register File Base. */
constexpr uintptr_t GRF_BASE = 0xFF288000U;

// -------------------------------------------------------------------------
// DWC2 Register Structures
// -------------------------------------------------------------------------

/**
 * @struct DevInEndpoint
 * @brief Device Logical IN Endpoint-Specific Registers (Offset: 0x900 + n*0x20).
 */
struct alignas(4) DevInEndpoint {
    volatile uint32_t diepctl;       /**< Device IN EP Control. */
    const uint32_t    reserved1;     /**< Offset: 0x04. */
    volatile uint32_t diepint;       /**< Device IN EP Interrupt. */
    const uint32_t    reserved2;     /**< Offset: 0x0C. */
    volatile uint32_t dieptsiz;      /**< Device IN EP Transfer Size. */
    volatile uint32_t diepdma;       /**< Device IN EP DMA Address. */
    const uint32_t    reserved3;     /**< Offset: 0x18. */
    volatile uint32_t diepdmab;      /**< Device IN EP DMA Buffer Address. */
};

/**
 * @struct DevOutEndpoint
 * @brief Device Logical OUT Endpoint-Specific Registers (Offset: 0xB00 + n*0x20).
 */
struct alignas(4) DevOutEndpoint {
    volatile uint32_t doepctl;       /**< Device OUT EP Control. */
    const uint32_t    reserved1;     /**< Offset: 0x04. */
    volatile uint32_t doepint;       /**< Device OUT EP Interrupt. */
    const uint32_t    reserved2;     /**< Offset: 0x0C. */
    volatile uint32_t doeptsiz;      /**< Device OUT EP Transfer Size. */
    volatile uint32_t doepdma;       /**< Device OUT EP DMA Address. */
    const uint32_t    reserved3;     /**< Offset: 0x18. */
    volatile uint32_t doepdmab;      /**< Device OUT EP DMA Buffer Address. */
};

/**
 * @struct ControllerRegs
 * @brief Full register layout of the Synopsys DWC2 controller.
 */
struct alignas(4) ControllerRegs {
    // ---------------------------------------------------------------------
    // 1. Core Global Registers (Offsets: 0x000 - 0x7FF)
    // ---------------------------------------------------------------------
    volatile uint32_t gotgctl;       /**< 0x000: OTG Control & Status. */
    volatile uint32_t gotgint;       /**< 0x004: OTG Interrupt. */
    volatile uint32_t gahbcfg;       /**< 0x008: Core AHB Configuration. */
    volatile uint32_t gusbcfg;       /**< 0x00C: Core USB Configuration. */
    volatile uint32_t grstctl;       /**< 0x010: Core Reset. */
    volatile uint32_t gintsts;       /**< 0x014: Core Interrupt Status. */
    volatile uint32_t gintmsk;       /**< 0x018: Core Interrupt Mask. */
    volatile uint32_t grxstsr;       /**< 0x01C: Rx Status Debug Read. */
    volatile uint32_t grxstsp;       /**< 0x020: Rx Status Debug Pop. */
    volatile uint32_t grxfsiz;       /**< 0x024: Receive FIFO Size. */
    volatile uint32_t gnptxfsiz;     /**< 0x028: Non-Periodic Tx FIFO Size. */
    const uint32_t    reserved0[3];
    volatile uint32_t ggpio;         /**< 0x038: General Purpose Input/Output. */
    const uint32_t    reserved1[5];
    volatile uint32_t ghwcfg4;       /**< 0x050: User HW Configuration 4. */
    const uint32_t    reserved2[44];
    volatile uint32_t dieptxf[15];    /**< 0x104 - 0x13C: Device Periodic Tx FIFO Sizes. */
    const uint32_t    reserved3[432];

    // ---------------------------------------------------------------------
    // 2. Device Configuration & Control Registers (Offsets: 0x800 - 0x8FF)
    // ---------------------------------------------------------------------
    volatile uint32_t dcfg;          /**< 0x800: Device Configuration. */
    volatile uint32_t dctl;          /**< 0x804: Device Control. */
    volatile uint32_t dsts;          /**< 0x808: Device Status. */
    const uint32_t    reserved4;
    volatile uint32_t diepmsk;       /**< 0x810: Device IN EP Common Interrupt Mask. */
    volatile uint32_t doepmsk;       /**< 0x814: Device OUT EP Common Interrupt Mask. */
    volatile uint32_t daint;         /**< 0x818: Device All Endpoints Interrupt. */
    volatile uint32_t daintmsk;      /**< 0x81C: Device All Endpoints Interrupt Mask. */
    const uint32_t    reserved5[56];

    // ---------------------------------------------------------------------
    // 3. Endpoint-Specific Registers (Offsets: 0x900 - 0xDFF)
    // ---------------------------------------------------------------------
    DevInEndpoint     in_endp[16];   /**< 0x900: Device Logical IN Endpoints (0-15). */
    DevOutEndpoint    out_endp[16];  /**< 0xB00: Device Logical OUT Endpoints (0-15). */
};

/** @brief Helper function to obtain the strongly-typed ControllerRegs pointer. */
inline ControllerRegs* get_regs() noexcept {
    return reinterpret_cast<ControllerRegs*>(USB_OTG0_BASE);
}


// -------------------------------------------------------------------------
// Register Bit Definitions (constexpr constants)
// -------------------------------------------------------------------------

namespace gotgctl {
    inline constexpr uint32_t B_SESSION_VALID = (1U << 19);
    inline constexpr uint32_t A_SESSION_VALID = (1U << 18);
    inline constexpr uint32_t B_VALOVAL       = (1U << 7);
    inline constexpr uint32_t B_VALOEN        = (1U << 6);
    inline constexpr uint32_t A_VALOVAL       = (1U << 5);
    inline constexpr uint32_t A_VALOEN        = (1U << 4);
}

namespace gotgint {
    inline constexpr uint32_t SES_END_DET     = (1U << 2);
}

namespace gahbcfg {
    inline constexpr uint32_t PTXFE_HALF      = (0U << 8);
    inline constexpr uint32_t PTXFE_ZERO      = (1U << 8);
    inline constexpr uint32_t NPTXFE_HALF     = (0U << 7);
    inline constexpr uint32_t NPTXFE_ZERO     = (1U << 7);
    inline constexpr uint32_t MODE_SLAVE      = (0U << 5);
    inline constexpr uint32_t MODE_DMA        = (1U << 5);
    inline constexpr uint32_t BURST_SINGLE    = (0U << 1);
    inline constexpr uint32_t BURST_INCR      = (1U << 1);
    inline constexpr uint32_t BURST_INCR4     = (3U << 1);
    inline constexpr uint32_t BURST_INCR8     = (5U << 1);
    inline constexpr uint32_t BURST_INCR16    = (7U << 1);
    inline constexpr uint32_t GBL_INT_UNMASK  = (1U << 0);
}

namespace grstctl {
    inline constexpr uint32_t AHB_MASTER_IDLE = (1U << 31);
    inline constexpr uint32_t TX_FIFO_FLUSH   = (1U << 5);
    inline constexpr uint32_t RX_FIFO_FLUSH   = (1U << 4);
    inline constexpr uint32_t CORE_SOFT_RESET = (1U << 0);
    
    constexpr uint32_t TX_FIFO_NUMBER(uint32_t x) noexcept {
        return x << 6;
    }
}

namespace gintsts {
    inline constexpr uint32_t INT_RESUME            = (1U << 31);
    inline constexpr uint32_t INT_DISCONN           = (1U << 29);
    inline constexpr uint32_t INT_CONN_ID_STS_CNG   = (1U << 28);
    inline constexpr uint32_t INT_OUT_EP            = (1U << 19);
    inline constexpr uint32_t INT_IN_EP             = (1U << 18);
    inline constexpr uint32_t INT_ENUMDONE          = (1U << 13);
    inline constexpr uint32_t INT_RESET             = (1U << 12);
    inline constexpr uint32_t INT_SUSPEND           = (1U << 11);
    inline constexpr uint32_t INT_EARLY_SUSPEND     = (1U << 10);
    inline constexpr uint32_t INT_NP_TX_FIFO_EMPTY  = (1U << 5);
    inline constexpr uint32_t INT_RX_FIFO_NOT_EMPTY = (1U << 4);
    inline constexpr uint32_t INT_SOF               = (1U << 3);
    inline constexpr uint32_t INT_OTG               = (1U << 2);
}

namespace dcfg {
    inline constexpr uint32_t DEV_SPEED_HIGH_SPEED_20 = (0U << 0);
    inline constexpr uint32_t DEV_SPEED_FULL_SPEED_20 = (1U << 0);
    inline constexpr uint32_t DCFG_DEVADDR_MASK       = (0x7FU << 4);
    
    constexpr uint32_t DEVICE_ADDRESS(uint32_t x) noexcept {
        return (x & 0x7FU) << 4;
    }
}

namespace dctl {
    inline constexpr uint32_t NORMAL_OPERATION = (1U << 0);
    inline constexpr uint32_t SOFT_DISCONNECT  = (1U << 1);
}

namespace epctl {
    inline constexpr uint32_t DEPCTL_EPENA      = (1U << 31);
    inline constexpr uint32_t DEPCTL_EPDIS      = (1U << 30);
    inline constexpr uint32_t DEPCTL_SETD1PID   = (1U << 29);
    inline constexpr uint32_t DEPCTL_SETD0PID   = (1U << 28);
    inline constexpr uint32_t DEPCTL_SNAK       = (1U << 27);
    inline constexpr uint32_t DEPCTL_CNAK       = (1U << 26);
    inline constexpr uint32_t DEPCTL_STALL      = (1U << 21);
    inline constexpr uint32_t DEPCTL_USBACTEP   = (1U << 15);
    inline constexpr uint32_t DEPCTL_MPS_MASK   = 0x7FFU;
    
    inline constexpr uint32_t DEPCTL0_MPS_64    = (0U << 0);
    inline constexpr uint32_t DEPCTL0_MPS_32    = (1U << 0);
    
    inline constexpr uint32_t DEPCTL_TYPE_BULK  = (2U << 18);
    inline constexpr uint32_t DEPCTL_TYPE_INTR  = (3U << 18);
    
    inline constexpr uint32_t DIEPCTL_TX_FIFO_NUM_MASK = ~(0xFU << 22);

    constexpr uint32_t DIEPCTL_TX_FIFO_NUM(uint32_t x) noexcept {
        return (x & 0xFU) << 22;
    }
}

namespace epint {
    inline constexpr uint32_t BACK2BACK_SETUP_RECEIVED     = (1U << 6);
    inline constexpr uint32_t CTRL_OUT_EP_SETUP_PHASE_DONE = (1U << 3); // For OUT EP0
    inline constexpr uint32_t AHB_ERROR                    = (1U << 2);
    inline constexpr uint32_t EPDISBLD                     = (1U << 1);
    inline constexpr uint32_t TRANSFER_DONE                 = (1U << 0);
}

namespace epsiz {
    constexpr uint32_t DIEPT_SIZ_PKT_CNT(uint32_t x) noexcept {
        return (x & 0x3FFU) << 19;
    }
    constexpr uint32_t DIEPT_SIZ_XFER_SIZE(uint32_t x) noexcept {
        return x & 0x7FFFFU;
    }
    /** SETUP packet count for doeptsiz0 (bits 30:29). */
    constexpr uint32_t DOEPT_SIZ_SUP_CNT(uint32_t x) noexcept {
        return (x & 0x3U) << 29;
    }

    inline constexpr uint32_t DOEPT_SIZ_XFER_SIZE_MAX_EP = 0x7FFFFU;
}

namespace gusbcfg {
    inline constexpr uint32_t FORCE_DEVICE_MODE = (1U << 30);
    inline constexpr uint32_t FORCE_HOST_MODE   = (1U << 29);
    inline constexpr uint32_t PHYIF_16BIT       = (1U << 3);

    /** UTMI turnaround time (bits 13:10); 5 for 16-bit UTMI+. */
    constexpr uint32_t USBTRDTIM(uint32_t x) noexcept {
        return (x & 0xFU) << 10;
    }
    inline constexpr uint32_t USBTRDTIM_MASK = (0xFU << 10);
}

namespace daint {
    /** IN endpoint bit (bits 15:0). */
    constexpr uint32_t IN_EP(uint32_t n) noexcept { return 1U << n; }
    /** OUT endpoint bit (bits 31:16). */
    constexpr uint32_t OUT_EP(uint32_t n) noexcept { return 1U << (16U + n); }
}

} // namespace rk3506::usb::dwc2

#endif // DWC2_REGS_HPP_
