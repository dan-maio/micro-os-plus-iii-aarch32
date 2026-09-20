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

#ifdef __cplusplus
}
#endif

#endif // CMSIS_DEVICE_H_
