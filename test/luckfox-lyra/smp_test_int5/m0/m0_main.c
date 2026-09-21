/*
 * m0_main.c — RK3506 Cortex-M0 firmware for smp_test_int5 (SysTick edition).
 *
 * Fires a mailbox doorbell to A7 cores 1 and 2 every ~40 ms:
 *   SysTick (200 MHz CPU clock) → Exception 15 (SysTick_Handler)
 *   ISR writes B2A DATA on mailbox channels 1 and 2, raising A7 SPI 171/172.
 */

#include <stdint.h>

/* ---- Mailbox (BB→AP), channels 1 and 2 ------------------------------------ */
#define MB_BASE(ch)        (0xFF290000U + (ch) * 0x1000U)
#define MB_B2A_INTEN(ch)   (*(volatile uint32_t *)(MB_BASE(ch) + 0x10))
#define MB_B2A_CMD(ch)     (*(volatile uint32_t *)(MB_BASE(ch) + 0x18))
#define MB_B2A_DATA(ch)    (*(volatile uint32_t *)(MB_BASE(ch) + 0x1C))

/* ---- Cortex-M0 SysTick Registers ------------------------------------------ */
#define SYSTICK_BASE       0xE000E010U
#define SYSTICK_CTRL       (*(volatile uint32_t *)(SYSTICK_BASE + 0x00))
#define SYSTICK_LOAD       (*(volatile uint32_t *)(SYSTICK_BASE + 0x04))
#define SYSTICK_VAL        (*(volatile uint32_t *)(SYSTICK_BASE + 0x08))

#define MASKED_WRITE(reg, mask, val) ((reg) = (((mask) << 16) | (val)))

static volatile uint32_t g_seq = 0;

static void mailbox_fire_both(void)
{
    uint32_t s = g_seq++;
    /* Channel 1 → A7 core 1 */
    MB_B2A_CMD(1)  = 0x0C010000U | (s & 0xFFFF);
    MB_B2A_DATA(1) = 0xABCD0000U | (s & 0xFFFF);   /* DATA write raises the SPI */
    /* Channel 2 → A7 core 2 */
    MB_B2A_CMD(2)  = 0x0C020000U | (s & 0xFFFF);
    MB_B2A_DATA(2) = 0x56780000U | (s & 0xFFFF);
}

/* SysTick ISR (exception 15) */
void SysTick_Handler(void)
{
    mailbox_fire_both();
}

static void m0_init(void)
{
    g_seq = 0;

    /* Enable both channels' B2A tx-done interrupt to the AP (normal mode) */
    MASKED_WRITE(MB_B2A_INTEN(1), 0x0101, 0x0101);
    MASKED_WRITE(MB_B2A_INTEN(2), 0x0101, 0x0101);

    /* Configure SysTick: 40 ms at 200 MHz core clock = 8,000,000 ticks.
     * SysTick counter is 24-bit, so 8,000,000 fits comfortably. */
    SYSTICK_LOAD = 8000000U - 1;
    SYSTICK_VAL  = 0;
    SYSTICK_CTRL = 0x07;   /* ENABLE=1, TICKINT=1, CLKSOURCE=1 */
}

int main(void)
{
    m0_init();
    for (;;) {
        __asm__ volatile("nop");
    }
}
