/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t os_port_clock_timestamp_t;
typedef uint32_t os_port_clock_duration_t;
typedef uint64_t os_port_clock_offset_t;

typedef bool os_port_scheduler_state_t;

typedef uint32_t os_port_irq_state_t;

typedef uint32_t os_port_thread_stack_element_t;
typedef uint64_t os_port_thread_stack_allocation_element_t;

typedef struct
{
  os_port_thread_stack_element_t* stack_ptr;
} os_port_thread_context_t;

/* SMP scheduler lock (recursive LDREX/STREX spinlock) */
typedef struct {
  volatile uint32_t lock;
  volatile uint32_t owner;
  volatile uint32_t depth;
} os_port_smp_klock_t;

/* SMP timer queue leaf lock (non-recursive) */
typedef struct {
  volatile uint32_t lock;
} os_port_smp_tlock_t;

#define SMP_NO_OWNER  0xFFFFFFFFu
/* The build chooses the core count (uos_add_app NCPU ...); four is the
 * BCM2837 default when nothing says otherwise. */
#ifndef OS_NCPU
#define OS_NCPU       4
#endif
/* BCM2837 has no GIC/SGIs; cross-core reschedule uses local mailbox 0.
 * Kept named OS_SMP_IPI_SGI for source compatibility with the a7 port. */
#define OS_SMP_IPI_SGI 0

// Select the SMP variant of the shared kernel (per-CPU current_thread_[OS_NCPU],
// affinity picker, per-core idle). The single-CPU Cortex-M build leaves this
// undefined and gets the stock scalar reference scheduler from the same kernel
// sources. Defined here (always included with OS_NCPU) so every kernel TU sees
// it regardless of OS_USE_OS_APP_CONFIG_H. See ../../../../
// micro-os-plus-iii-cortexm-nucleof411/PLAN.md.
#if !defined(OS_USE_SMP_SCHEDULER) && (OS_NCPU > 1)
#define OS_USE_SMP_SCHEDULER 1
#endif
#define OS_INTEGER_RTOS_STACK_FILL_MAGIC (0xEFBEADDE)

#define OS_HAS_INTERRUPTS_STACK

#endif /* CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_ */
