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

// A harness suite built as a board app (test/<board>/<suite>, wired by
// board_test_libs()) carries its own os-app-config.h, and on every harness
// platform that file IS the configuration -- rtos-apis needs its thread
// statistics and its 512 KiB RTOS arena, mutex-stress the same shape. This
// port's include directory is searched first (it comes with
// micro-os-plus::aarch32), so it is this file the kernel finds: hand over to
// the suite's under the harness marker. A port test defines no
// UOS_HARNESS_SUITE and gets exactly the (empty) values below -- the same
// arrangement the Cortex-M pico2 board uses to keep its own defaults while
// letting the suite's settings win.
#if defined(UOS_HARNESS_SUITE)
#include_next <cmsis-plus/os-app-config.h>
#endif

// (Configuration is provided via -D flags and port/os-c-decls.h, where
// OS_USE_SMP_SCHEDULER is defined next to OS_NCPU.)

#endif /* CMSIS_PLUS_OS_APP_CONFIG_H_ */
