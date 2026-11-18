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
set (BOARD_TEST_NEED_DEVICES "")
# usb_test reaches the card through µOS++ POSIX I/O and Chan FatFs (mounted at
# /sd/ by uos_test_storage_start()); its USB gadget is
# micro-os-plus::devices-usb (board_test_libs below).
list (APPEND UOS_TESTS_STORAGE usb_test)

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
# The harness suites (mutex-stress, rtos-apis, cmsis-os-validator) are no
# longer here: they are upstream's, built by the tests' builder from
# tests/sources/<suite>/ (the platform's UOS_TESTS_SUITES), with the
# validator's board glue in platforms/<P>/src/cmsis-os-validator-support.cpp.

function (board_test_libs _app _out)
  if (_app STREQUAL "usb_test")
    set (${_out} micro-os-plus::devices-usb PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()
