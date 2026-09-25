# -----------------------------------------------------------------------------
# What this board's tests need.
#
# Which tests exist is the directory listing beside this file -- a board builds
# what it has, so there is no capability filter and no list of names to keep in
# step with the directories. This file carries only the knobs a test cannot
# state for itself.
# -----------------------------------------------------------------------------

# Tests that reach the SD card or the USB device controller, and so link
# UOS_BOARD_DEVICES (boards/<id>/board.cmake names which driver set that is).
set (BOARD_TEST_NEED_DEVICES
     sd_test smp-mat-sdcard-test smp-num-test smp-pipeline-test usb_test)

# Per-test -D flags, carried over from the per-test Makefiles.
function (board_test_defines _app _out)
  set (_d "")
  if (_app STREQUAL "usb_test")
    # usb_test's own blink pattern: LED_BLINKS blinks of LED_ON_MS lit and
    # LED_OFF_MS dark, then LED_GAP_MS dark, repeating. Which GPIO that is
    # belongs to the board, so LED_PIN is set for every test, not here.
    list (APPEND _d LED_BLINKS=3 LED_ON_MS=40 LED_OFF_MS=40
                    LED_GAP_MS=300 USB_FORCE_FS)
  elseif (_app STREQUAL "cmsis-os-validator")
    # The validator is Cortex-M code. For its image only, the board (the
    # Zero 2 W's sources, boards/rpi-zero-2w/src/) maps a "no cycle counter"
    # page where it probes the DWT (mmu.cpp), and routes Mailbox 1, its
    # stand-in for NVIC IRQ 0, to its handler (rtos/port_isr.cpp). Every other
    # test's image is unchanged.
    list (APPEND _d UOS_CMSIS_OS_VALIDATOR)
  endif ()
  set (${_out} "${_d}" PARENT_SCOPE)
endfunction ()

# -----------------------------------------------------------------------------
# The harness suites.
#
# They are not board tests: their sources live in the harness,
# tests/sources/<suite>/, and the directory under test/rpi3b/ carries only the
# name. The harness's platform add_subdirectory()s those sources into
# test::<suite>, so naming that library below brings its sources AND its
# include directory -- including the suite's own cmsis-plus/os-app-config.h,
# which is why the directory must stay off the board's include path.
#
# Everything else is a board test's: the port's builder compiles the directory,
# its src/ support (test/rpi3b/src/test-smp-boot.cpp brings the secondary cores
# up) and the board's runtime, exactly as for smp_test1..4. The only additions
# are what a suite needs and a board test does not, gathered in one INTERFACE
# target so each suite names it once:
#
#   OS_USE_OS_APP_CONFIG_H  the kernel headers read the suite's own
#                           cmsis-plus/os-app-config.h only under it. Without
#                           it the kernel uses its default config, and rtos-apis
#                           dies in malloc_memory_resource() before it starts;
#   UOS_HARNESS_SUITE       the marker the harness suites share (kept for
#                           symmetry with the Cortex-M boards, whose board
#                           os-app-config.h include_next's the suite's);
#   the platform include    that os-app-config.h includes cmsis-plus/platform.h,
#                           which is the harness platform's -- the one this
#                           port's builder is add_subdirectory()'d from;
#   platform-support.cpp    the startup hooks and strong main() the suite does
#                           not supply. The port's own tests each write their
#                           own main(); the suite has none, and the kernel's
#                           weak main never sets the interrupts stack;
#   the syscall groups      the suites call gettimeofday() and the POSIX I/O
#                           layer (micro-os-plus::iii-posix-io);
#   --wrap=os_main          the verdict. A suite RETURNS its code from os_main();
#                           hw.sh reads a RESULT line. Each placeholder
#                           harness-suite.cpp defines __wrap_os_main(), which
#                           calls the suite, prints that line on the UART and on
#                           the semihosting channel, and then stops the run
#                           through semihosting::report_result() -- the same
#                           SYS_EXIT the port's tests end with. On this SMP port
#                           it deliberately does not return through the
#                           trampoline's std::exit(): that runs the atexit
#                           handlers with the suite's threads still alive and
#                           has been observed to block.
#
# A suite exists only inside the harness -- test::<suite> is the harness's --
# so PLATFORM_NAME and the harness's platforms/ are always there when this is.
if (NOT TARGET aarch32-rpi3b-harness-suite)
  add_library (aarch32-rpi3b-harness-suite INTERFACE)
  target_compile_definitions (aarch32-rpi3b-harness-suite INTERFACE
                              OS_USE_OS_APP_CONFIG_H UOS_HARNESS_SUITE
                              _POSIX_C_SOURCE=200809L _GNU_SOURCE)
  target_include_directories (aarch32-rpi3b-harness-suite INTERFACE
                              "${CMAKE_SOURCE_DIR}/platforms/${PLATFORM_NAME}/include")
  # The suite supplies no main() and no startup hooks; this is the harness
  # platform's, compiled into the application with the builder's variant
  # defines (QEMU_BUILD/HW_BUILD), so one copy serves both images.
  target_sources (aarch32-rpi3b-harness-suite INTERFACE
                  "${CMAKE_SOURCE_DIR}/platforms/${PLATFORM_NAME}/src/platform-support.cpp")
  target_link_libraries (aarch32-rpi3b-harness-suite INTERFACE
                         micro-os-plus::iii-semihosting
                         micro-os-plus::iii-newlib-reent
                         micro-os-plus::iii-posix-io)
  target_link_options (aarch32-rpi3b-harness-suite INTERFACE "-Wl,--wrap=os_main")
endif ()

function (board_test_libs _app _out)
  if (_app STREQUAL "rtos-apis")
    set (${_out} test::rtos-apis xpacks::chan-fatfs
         aarch32-rpi3b-harness-suite PARENT_SCOPE)
  elseif (_app STREQUAL "mutex-stress")
    set (${_out} test::mutex-stress aarch32-rpi3b-harness-suite PARENT_SCOPE)
  elseif (_app STREQUAL "cmsis-os-validator")
    set (${_out} test::cmsis-os-validator xpacks::arm-cmsis-os-validator
         aarch32-rpi3b-harness-suite PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()
