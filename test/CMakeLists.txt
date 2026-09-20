# -----------------------------------------------------------------------------
# Test builds for the AArch32 port.
#
# Twelve applications x two variants = 24 targets, all from one loop. The
# application list and the per-test knobs come from the kernel repository
# (test/apps.cmake), so this file never names a test.
#
#   qemu  the build the QEMU suites run: semihosting on, no boot-card I/O
#   hwd   the build for real hardware: HW_BUILD, so the SD tests use the
#         existing FAT32 boot partition, plus the early-boot asm markers
#
# BOARD selects the silicon: zero2w (default) or rpi3b.
# -----------------------------------------------------------------------------

set (BOARD "zero2w" CACHE STRING "Target board: zero2w or rpi3b")
set_property (CACHE BOARD PROPERTY STRINGS zero2w rpi3b)

set (_boards_dir "${UOS_AARCH32_ROOT}/boards/rpi-zero-2w")
if (BOARD STREQUAL "rpi3b")
  set (_linker "${_boards_dir}/linker-rpi3b.ld")
  set (_board_defines BOARD_RPI3B)
else ()
  set (_linker "${_boards_dir}/linker.ld")
  set (_board_defines "")
endif ()

# Shared by every test on this port. Semihosting is on by default, as it was
# in the Makefiles: the console reaches the emulator or the OpenOCD log, and a
# test ends its run with a semihosted exit as soon as it has a RESULT.
set (_common_defines TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST ${_board_defines})

foreach (_app IN LISTS UOS_TEST_APPS)
  uos_test_app_defines ("${_app}" _app_defines)

  set (_libs micro-os-plus::aarch32)
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_SD OR "${_app}" STREQUAL "usb_test")
    list (APPEND _libs micro-os-plus::devices)
  endif ()

  foreach (_variant IN ITEMS qemu hwd)
    if (_variant STREQUAL "hwd")
      set (_variant_defines HW_BUILD DEBUG_BOOT)
    else ()
      set (_variant_defines "")
    endif ()

    uos_add_test_app (
      "${_app}-${_variant}"
      APP "${_app}"
      NCPU 4
      LINKER_SCRIPT "${_linker}"
      DEFINES ${_common_defines} ${_app_defines} ${_variant_defines}
      LIBRARIES ${_libs}
      OUTPUT_NAME "${_app}-${_variant}"
    )
  endforeach ()
endforeach ()

list (LENGTH UOS_TEST_APPS _n)
math (EXPR _n "${_n} * 2")
message (STATUS "µOS++ III AArch32 tests: ${_n} targets, board=${BOARD}")
