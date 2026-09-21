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
  endif ()
  set (${_out} "${_d}" PARENT_SCOPE)
endfunction ()
