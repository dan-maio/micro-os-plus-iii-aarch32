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

/* OS_NCPU and OS_SMP_IPI_SGI are BOARD facts and have no default here.
 *
 * They used to. OS_NCPU defaulted to 4 -- the first board's core count -- so
 * a board whose build forgot to say would have silently got a four-CPU SMP
 * scheduler. On a single-core board that is not a wrong number, it is a
 * different operating system. OS_SMP_IPI_SGI was hard-coded to 0, which
 * means "local mailbox 0" on a BCM2837 and "GIC SGI 0" on an RK3506: two
 * unrelated things that happened to share a number, so the second board
 * worked by luck rather than by agreement.
 *
 * Both now come from boards/<board>/board.cmake, and a board that does not
 * state them does not compile. See src/board-contract.cpp. */
#ifndef OS_NCPU
#error "OS_NCPU is a board fact: set UOS_BOARD_NCPU in boards/<board>/board.cmake."
#endif
#if (OS_NCPU > 1) && !defined(OS_SMP_IPI_SGI)
#error "OS_SMP_IPI_SGI is a board fact: add it to UOS_BOARD_DEFINES in boards/<board>/board.cmake -- the interrupt a core uses to ask another to reschedule."
#endif

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
