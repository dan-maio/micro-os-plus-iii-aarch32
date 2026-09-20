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
option (UOS_DEBUG_BOOT "Compile the hwd builds with the early-boot markers" OFF)
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
      # DEBUG_BOOT is opt-in, as the sources themselves assume ("default
      # OFF, so the normal build stays quiet"): every early-boot marker
      # is a semihosting trap, and under a JTAG probe each one costs real
      # time. Turn it on only when bringing a board up.
      set (_variant_defines HW_BUILD)
      if (UOS_DEBUG_BOOT)
        list (APPEND _variant_defines DEBUG_BOOT)
      endif ()
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

# -----------------------------------------------------------------------------
# QEMU boot shim.
#
# Real hardware boots this port directly: config.txt sets arm_64bit=0 and the
# Cortex-A53 cores come up in AArch32. QEMU's raspi3b machine does not offer
# that, so a 20-line AArch64 stub drops to AArch32 and jumps to the image.
# Needed only to run the suite; nothing links against it.
#
# It is built with the *AArch64* toolchain, which is why it is a custom command
# rather than a target of this project.
file (GLOB _a64_bins "$ENV{HOME}/.local/xPacks/@xpack-dev-tools/aarch64-none-elf-gcc/*/.content/bin")
if (_a64_bins)
  list (SORT _a64_bins COMPARE NATURAL ORDER DESCENDING)
  list (GET _a64_bins 0 _a64_bin)
  set (_shim_src "${_boards_dir}/qemu-raspi3-shim")
  add_custom_command (
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shim8.img"
    COMMAND "${_a64_bin}/aarch64-none-elf-gcc" -nostdlib
            "-Wl,-T,${_shim_src}/shim.ld" "${_shim_src}/shim.S"
            -o "${CMAKE_CURRENT_BINARY_DIR}/shim.elf"
    COMMAND "${_a64_bin}/aarch64-none-elf-objcopy" -O binary
            "${CMAKE_CURRENT_BINARY_DIR}/shim.elf"
            "${CMAKE_CURRENT_BINARY_DIR}/shim8.img"
    DEPENDS "${_shim_src}/shim.S" "${_shim_src}/shim.ld"
    COMMENT "Building the AArch64->AArch32 QEMU boot shim"
    VERBATIM
  )
  add_custom_target (qemu-shim ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/shim8.img")
else ()
  message (STATUS "µOS++ III AArch32: no aarch64 toolchain, QEMU boot shim not built")
endif ()
