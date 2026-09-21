# -----------------------------------------------------------------------------
# Test builds for the AArch32 port.
#
# Twelve applications x two variants = 24 targets, all from one loop. The
# application list and the per-test knobs come from the kernel repository
# (test_smpl/apps.cmake), so this file never names a test.
#
#   qemu  the build the QEMU suites run: semihosting on, no boot-card I/O
#   hwd   the build for real hardware: HW_BUILD, so the SD tests use the
#         existing FAT32 boot partition, plus the early-boot asm markers
#
# BOARD selects the board: rpi-zero-2w (default), rpi3b or luckfox-lyra.
# -----------------------------------------------------------------------------

# BOARD is declared by the port, above: it picks the board directory the port
# compiles, so the port needs it before this directory is added.
option (UOS_DEBUG_BOOT "Compile the hwd builds with the early-boot markers" OFF)

set (_boards_dir "${UOS_BOARD_SRC_DIR}")

# Every board fact this file needs was declared by boards/<BOARD>/board.cmake
# -- the linker script per variant, the core count, the LED pin, the defines.
# There is deliberately no "if (BOARD STREQUAL ...)" anywhere below: a port
# with twenty boards cannot carry a chain per file, and a chain is exactly
# what lets a new board silently take the previous one's branch.
set (_linker_hwd    "${UOS_BOARD_LINKER_HW}")
set (_linker_qemu   "${UOS_BOARD_LINKER_QEMU}")
set (_ncpu          "${UOS_BOARD_NCPU}")
set (_board_defines "")

set (_common_defines TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST ${_board_defines})

# Which of the shared tests this board can build at all.
#
# Asked as a CAPABILITY question, never as a board question. A test declares
# what it needs (apps.cmake); a board declares what it has (board.cmake); a
# test is built when the board has everything it needs.
#
# So a board with no SD card, no USB device controller, or only one CPU needs
# no edit here -- and on a single-CPU port the SMP-only tests simply do not
# exist, rather than being built and then failing.
set (_apps "")
foreach (_app IN LISTS UOS_TEST_APPS)
  set (_needs "")
  if ("${_app}" IN_LIST UOS_TEST_APPS_SMP_ONLY)
    list (APPEND _needs smp)
  endif ()
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_SD)
    list (APPEND _needs sdcard)
  endif ()
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_USB)
    list (APPEND _needs usb-device)
  endif ()

  set (_buildable TRUE)
  foreach (_cap IN LISTS _needs)
    if (NOT "${_cap}" IN_LIST UOS_BOARD_CAPS)
      set (_buildable FALSE)
    endif ()
  endforeach ()
  if (_buildable)
    list (APPEND _apps "${_app}")
  endif ()
endforeach ()

foreach (_app IN LISTS _apps)
  uos_test_app_defines ("${_app}" _app_defines)

  set (_libs micro-os-plus::aarch32)
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_SD
      OR "${_app}" IN_LIST UOS_TEST_APPS_NEED_USB)
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
      set (_variant_linker "${_linker_hwd}")
    else ()
      # QEMU_BUILD says "this image runs under the emulator", which is not
      # the same as "not hardware": a board whose silicon QEMU does not
      # model builds its emulated image against the `virt` machine instead.
      set (_variant_defines QEMU_BUILD)
      set (_variant_linker "${_linker_qemu}")
    endif ()

    uos_add_test_app (
      "${_app}-${_variant}"
      APP "${_app}"
      NCPU ${_ncpu}
      LINKER_SCRIPT "${_variant_linker}"
      DEFINES ${_common_defines} ${_app_defines} ${_variant_defines}
      LIBRARIES ${_libs}
      OUTPUT_NAME "${_app}-${_variant}"
    )
  endforeach ()
endforeach ()

list (LENGTH _apps _n)
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
#
# It is also purely a Pi problem. The Luckfox Lyra runs under QEMU's `virt`
# machine, whose Cortex-A7 cores start in AArch32 because that is the only
# state an A7 has -- so there is nothing to drop out of, and no shim.
file (GLOB _a64_bins "$ENV{HOME}/.local/xPacks/@xpack-dev-tools/aarch64-none-elf-gcc/*/.content/bin")
if (_a64_bins AND EXISTS "${_boards_dir}/qemu-raspi3-shim/shim.S")
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
