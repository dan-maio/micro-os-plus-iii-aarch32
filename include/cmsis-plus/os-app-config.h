/*
 * os-app-config.h — application configuration for the RK3506 Cortex-A7 SMP port.
 *
 * micro-os-plus-iii `develop` includes this header unconditionally from
 * <cmsis-plus/rtos/os-c-decls.h> (unlike v7.0.1, which did not). All real
 * configuration for this port is supplied via the build's -D DEFINES and the
 * port header <cmsis-plus/rtos/port/os-c-decls.h> (which defines OS_NCPU=3,
 * the scheduler mode, etc.), so this file intentionally holds no macros — it
 * exists only to satisfy the kernel's mandatory include on develop.
 */

#ifndef CMSIS_PLUS_OS_APP_CONFIG_H_
#define CMSIS_PLUS_OS_APP_CONFIG_H_

// (Configuration is provided via -D flags and port/os-c-decls.h, where
// OS_USE_SMP_SCHEDULER is defined next to OS_NCPU.)

#endif /* CMSIS_PLUS_OS_APP_CONFIG_H_ */
