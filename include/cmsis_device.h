#ifndef CMSIS_DEVICE_H_
#define CMSIS_DEVICE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

static inline void NVIC_SystemReset(void) {
  while(1);
}

#define __NOP() __asm__ volatile ("nop")
#define __BKPT(n) __asm__ volatile ("bkpt 0")

// No NVIC on the Cortex-A53: a board that runs the CMSIS-RTOS validator
// defines these on its native interrupt controller (the Zero 2 W: BCM2837
// Mailbox 1, test/rpi-zero-2w/cmsis-os-validator/harness-suite.cpp).
typedef int IRQn_Type;

void NVIC_EnableIRQ(IRQn_Type irq);
void NVIC_DisableIRQ(IRQn_Type irq);
void NVIC_SetPendingIRQ(IRQn_Type irq);

extern uint32_t SystemCoreClock;
static inline void SystemCoreClockUpdate(void) {}

#ifdef __cplusplus
}
#endif

#endif // CMSIS_DEVICE_H_
