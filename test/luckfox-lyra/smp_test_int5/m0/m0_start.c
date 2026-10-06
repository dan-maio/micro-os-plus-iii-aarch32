/*
 * m0_start.c — Cortex-M0 vector table + reset for smp_test_int5 M0 firmware.
 *
 * The M0 boots by fetching MSP from offset 0x00 and the reset PC from 0x04 of
 * its vector table, which the linker places at the image base (0x00000000 in
 * the M0's address space; see linker.ld and openocd_m0.cfg for the boot-address
 * discovery note). Minimal: only the vectors we use are named, the rest point
 * at a spin loop.
 */

#include <stdint.h>

extern int  main(void);
extern void SysTick_Handler(void);              /* m0_main.c — SysTick ISR */
extern uint32_t _estack;                        /* top of stack (linker) */

static void Default_Handler(void) { for (;;) { } }
void Reset_Handler(void);

/* Vector table: [0]=MSP, [1]=Reset, [2..15]=system, [16+n]=external IRQn.
 * SysTick sits at index 15. */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))&_estack,   /* 0  Initial MSP                       */
    Reset_Handler,              /* 1  Reset                             */
    Default_Handler,            /* 2  NMI                               */
    Default_Handler,            /* 3  HardFault                         */
    0, 0, 0, 0, 0, 0, 0,        /* 4..10 reserved                       */
    Default_Handler,            /* 11 SVC                               */
    0, 0,                       /* 12..13 reserved                      */
    Default_Handler,            /* 14 PendSV                            */
    SysTick_Handler,            /* 15 SysTick                           */
    /* External IRQ0..IRQ31 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 0-3   */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 4-7   */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 8-11  */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 12-15 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 16-19 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 20-23 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 24-27 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 28-31 */
};

void Reset_Handler(void)
{
    main();
    for (;;) { }
}
