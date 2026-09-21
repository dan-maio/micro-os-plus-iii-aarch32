# -----------------------------------------------------------------------------
# What this board's tests need.
#
# Which tests exist is the directory listing beside this file. usb_test is not
# among them, because the RK3506 DWC2 device stack has not been carried over
# from the predecessor yet -- so it is absent rather than filtered out, and
# nothing here has to know why.
#
# smp_test_int and smp_test_int2 are this board's own, carried over byte for
# byte: they exercise the GIC-400 SGI paths, which no other board has.
# -----------------------------------------------------------------------------

set (BOARD_TEST_NEED_DEVICES
     sd_test smp-mat-sdcard-test smp-num-test smp-pipeline-test)

# Carried over whole from the predecessor, each with its own start-up code.
set (BOARD_TEST_SELF_CONTAINED smp_test_int smp_test_int2)

function (board_test_defines _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
