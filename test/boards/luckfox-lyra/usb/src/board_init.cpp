/**
 * @file board_init.cpp
 * @brief RK3506 Board / hardware initialization.
 *
 * Implements low-level UART1 console initialization, GIC interrupt dispatching,
 * MMU configuration, memory checks, and the main entry loop.
 */

#include <stdint.h>
#include "dwc2_driver.hpp"

// Register read/write helpers
static inline void writel(uint32_t val, uintptr_t addr) {
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t readl(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

// Hardware Constants for RK3506
// Console is UART1 (0xFF0B0000): UART0's pins (GPIO0_C6/C7) carry the SWD
// probe on this board, so UART0 output can never leave the chip. UART1 is
// routed through RM_IO onto GPIO0_B4/B5 (Lyra debug header). Init sequence
// ported from custom_bootloader/src/uart1.c (proven on this board).
static constexpr uintptr_t UART1_BASE        = 0xFF0B0000U;
static constexpr uint32_t  UART_THR          = 0x00U;  // TX holding (write)
static constexpr uint32_t  UART_DLL          = 0x00U;  // divisor low  (DLAB=1)
static constexpr uint32_t  UART_DLH          = 0x04U;  // divisor high (DLAB=1)
static constexpr uint32_t  UART_IER          = 0x04U;
static constexpr uint32_t  UART_FCR          = 0x08U;
static constexpr uint32_t  UART_LCR          = 0x0CU;
static constexpr uint32_t  UART_LSR          = 0x14U;
static constexpr uint32_t  UART_LSR_THRE     = 0x20U;
static constexpr uint32_t  LCR_DLAB          = (1U << 7);
static constexpr uint32_t  LCR_WLS_8BIT      = 0x03U;
static constexpr uint32_t  FCR_FIFO_EN       = 0x07U;  // FIFO en + RX/TX reset
static constexpr uint32_t  UART1_REF_CLK_HZ  = 24000000U;
static constexpr uint32_t  UART1_BAUD        = 115200U;

// Pinmux / clock plumbing for UART1 (see custom_bootloader/src/regs.h)
static constexpr uintptr_t PMUGRF_BASE          = 0xFF910000U; // RM_IO matrix
static constexpr uintptr_t IOC_BASE             = 0xFF950000U;
static constexpr uintptr_t CRU_BASE             = 0xFF9A0000U;
static constexpr uintptr_t GPIO0B_IOMUX_SEL_1   = IOC_BASE   + 0x000CU;
static constexpr uintptr_t RM_IO_GPIO0B4_SEL    = PMUGRF_BASE + 0x00B0U;
static constexpr uintptr_t RM_IO_GPIO0B5_SEL    = PMUGRF_BASE + 0x00B4U;
static constexpr uintptr_t CRU_CLKSEL_CON30     = CRU_BASE   + 0x0378U;
static constexpr uintptr_t CRU_GATE_CON11       = CRU_BASE   + 0x082CU;
static constexpr uintptr_t CRU_SOFTRST_CON11    = CRU_BASE   + 0x0A2CU;

// Rockchip masked write: upper 16 bits = write-enable mask
static inline void masked_write(uintptr_t addr, uint32_t mask, uint32_t val) {
    *(volatile uint32_t *)addr = (mask << 16) | val;
}

static constexpr uintptr_t GIC_CPU_BASE      = 0xFF582000U; // RK3506 GIC-400 CPU Base
static constexpr uintptr_t GICC_IAR_OFF      = 0x000CU;
static constexpr uintptr_t GICC_EOIR_OFF     = 0x0010U;
static constexpr uint32_t  USB0_IRQ_INTID    = 106U;       // SPI 74 + 32 = 106

// External symbols
extern "C" {
    void init_gd(void);
    void increment_timer(void);
    void uart_putc(char c);
    void uart_puts(const char *s);
    char *hex32(uint32_t val);
}

void mmu_init(void);
int  usb_gadget_init(void);
void usb_vendor_start_read_queue(void);
#ifdef USB_BRIDGE
extern "C" void usb1_device_init(void);
extern "C" void usb1_poll(void);
#endif

extern bool device_connected;
extern bool endpoints_enabled;
extern bool read_pending;

extern "C" {

void uart_putc(char c) {
    while (!(readl(UART1_BASE + UART_LSR) & UART_LSR_THRE));
    writel(static_cast<uint32_t>(c), UART1_BASE + UART_THR);
    if (c == '\n') {
        uart_putc('\r');
    }
}

/**
 * @brief Full standalone UART1 console bring-up: RM_IO pinmux, clocks, 8N1.
 */
static void uart1_init(void) {
    // Pinmux: hand GPIO0_B4/B5 pads to RM_IO (function 7), then point the
    // matrix selects at UART1_TX (idx 1) / UART1_RX (idx 2).
    masked_write(GPIO0B_IOMUX_SEL_1, 0x00FFU, 0x0077U);
    masked_write(RM_IO_GPIO0B4_SEL,  0x007FU, 0x0001U);
    masked_write(RM_IO_GPIO0B5_SEL,  0x007FU, 0x0002U);

    // Clock: sclk_uart1 = 24 MHz osc, div 1; un-gate pclk/sclk; pulse reset.
    masked_write(CRU_CLKSEL_CON30, 0x00FFU, 0x0000U);
    masked_write(CRU_GATE_CON11,   0x0420U, 0x0000U);
    masked_write(CRU_SOFTRST_CON11, 0x0420U, 0x0420U);
    for (volatile int i = 0; i < 2000; ++i) { __asm__("nop"); }
    masked_write(CRU_SOFTRST_CON11, 0x0420U, 0x0000U);

    // Line setup: divisor for 115200, 8N1, FIFOs on, no UART interrupts.
    uint32_t div = UART1_REF_CLK_HZ / (16U * UART1_BAUD);
    if (div == 0U) div = 1U;
    writel(LCR_DLAB, UART1_BASE + UART_LCR);
    writel(div & 0xFFU, UART1_BASE + UART_DLL);
    writel((div >> 8) & 0xFFU, UART1_BASE + UART_DLH);
    writel(LCR_WLS_8BIT, UART1_BASE + UART_LCR);
    writel(FCR_FIFO_EN, UART1_BASE + UART_FCR);
    writel(0x00U, UART1_BASE + UART_IER);
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}

char *hex32(uint32_t val) {
    static char bufs[4][11];
    static unsigned idx = 0;
    char *buf = bufs[idx++ & 3u];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = "0123456789ABCDEF"[val & 0xFU];
        val >>= 4;
    }
    buf[10] = '\0';
    return buf;
}

/**
 * @brief Top-level IRQ dispatcher.
 */
void irq_handler(void) {
    auto* gicc_iar  = reinterpret_cast<volatile uint32_t*>(GIC_CPU_BASE + GICC_IAR_OFF);
    auto* gicc_eoir = reinterpret_cast<volatile uint32_t*>(GIC_CPU_BASE + GICC_EOIR_OFF);

    uint32_t iar   = *gicc_iar;
    uint32_t intid = iar & 0x3FFU;

    if (intid == USB0_IRQ_INTID) {
        rk3506::usb::dwc2::Driver::instance().handle_interrupt();
    }

    *gicc_eoir = iar;
}

/**
 * @brief Main application boot/poll execution path.
 */
void main_entry(void) {
    // 0. Console first so every later stage can report
    uart1_init();

    // 1. Initialize global data and memory management
    init_gd();
    mmu_init();
    
    // 2. Enable SMP mode in ACTLR (needed for DMA cache coherency operations)
    {
        uint32_t actlr;
        __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
        actlr |= (1U << 6); // SMP bit
        __asm__ __volatile__("mcr p15, 0, %0, c1, c0, 1" : : "r"(actlr));
    }

    uart_puts("\nLuckfox Lyra (RK3506) USB Bare-Metal Test (C++23)\n");
    uart_puts("----------------------------------------------\n");

    // 3. Perform a SDRAM sanity write/read test
    {
        volatile uint32_t *test_addr = (volatile uint32_t *)0x04000000U;
        uint32_t wval = 0x55AA33CCU;
        *test_addr = wval;
        uint32_t rval = *test_addr;

        uart_puts("SDRAM Test (0x04000000): write=");
        uart_puts(hex32(wval));
        uart_puts(" read=");
        uart_puts(hex32(rval));
        uart_puts(wval == rval ? " [PASS]\n" : " [FAIL]\n");
    }

    // 4. Initialize USB low-level DWC2 peripheral driver & GIC routing
    uart_puts("Initializing DWC2 Controller & PHY... ");
    if (rk3506::usb::dwc2::Driver::instance().initialize() != rk3506::usb::dwc2::DriverStatus::OK) {
        uart_puts("[FAILED]\n");
        while (1);
    }
    uart_puts("[OK]\n");

    // 5. Initialize high-level gadget composite framework
    uart_puts("Registering Custom USB Gadget Interface... ");
    if (usb_gadget_init()) {
        uart_puts("[FAILED]\n");
        while (1);
    }
    uart_puts("[OK]\n");

    uart_puts("System online. Waiting for USB Host connection...\n");
    uart_puts("BUILD: "
#ifdef USB_BRIDGE
              "bridge"
#else
              "usb0"
#endif
              " / "
#ifdef USB_FORCE_FS
              "FULL-SPEED forced"
#else
              "high-speed"
#endif
              "\n");

#ifdef USB_BRIDGE
    // 5b. Bring up the second controller (OTG1, device mode toward PC1)
    usb1_device_init();
#endif

    // 6. Main Poll Loop
    uint32_t poll_count = 0;
    while (1) {
        // Poll driver in case interrupts are masked or edge missed
        rk3506::usb::dwc2::Driver::instance().handle_interrupt();
#ifdef USB_BRIDGE
        usb1_poll();
#endif
        increment_timer();

        if (device_connected && endpoints_enabled) {
            if (!read_pending) {
                usb_vendor_start_read_queue();
            }
        }

        poll_count++;
        if (poll_count >= 1000000) {
            poll_count = 0;
            // Activity heartbeat
            uart_putc('.');
        }
    }
}

} // extern "C"
