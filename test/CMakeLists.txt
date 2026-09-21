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
# BOARD selects the silicon: zero2w (default), rpi3b or luckfox-lyra.
# -----------------------------------------------------------------------------

# BOARD is declared by the port, above: it picks the board directory the port
# compiles, so the port needs it before this directory is added.
option (UOS_DEBUG_BOOT "Compile the hwd builds with the early-boot markers" OFF)

set (_boards_dir "${UOS_AARCH32_ROOT}/boards/${UOS_BOARD_DIR}")
# Which LED a test blinks is a fact about the board, not about the test, so
# it is set here for every test rather than per application.
#
#   zero2w  the onboard green ACT LED is a plain SoC GPIO, number 29.
#           led.hpp's own fallback is GPIO16 -- header pin 36, for an
#           externally wired LED -- which blinks nothing on a bare board.
#   rpi3b   the ACT LED is not a SoC GPIO at all: it is VideoCore expander
#           GPIO 130, driven through the mailbox, so LED_PIN does not apply.
set (LED_PIN 29 CACHE STRING "GPIO driven as the user LED (zero2w only)")

# Every other board fact a test build needs: where the image is linked for
# each variant, how many CPUs the scheduler has, and the board #define.
if (BOARD STREQUAL "rpi3b")
  set (_linker_hwd  "${_boards_dir}/linker-rpi3b.ld")
  set (_linker_qemu "${_boards_dir}/linker-rpi3b.ld")
  set (_board_defines BOARD_RPI3B)
  set (_ncpu 4)
elseif (BOARD STREQUAL "luckfox-lyra")
  # Three CPUs, not four. The RK3506 also carries a Cortex-M0, but it is a
  # different ISA outside the A7 coherency domain: it cannot take an IPI as
  # a peer and the scheduler cannot place a thread on it. It is the board
  # that is multi-core; the SMP cluster is the three identical A7s.
  #
  # The emulated runs use QEMU's `virt` machine -- generic Cortex-A7, RAM at
  # 0x4000_0000 -- because none of the RK3506's own blocks are modelled. The
  # hardware runs link for the Lyra's own DRAM map.
  set (_linker_hwd  "${_boards_dir}/linker.ld")
  set (_linker_qemu "${_boards_dir}/linker-qemu-virt.ld")
  set (_board_defines "")
  set (_ncpu 3)
else ()
  set (_linker_hwd  "${_boards_dir}/linker.ld")
  set (_linker_qemu "${_boards_dir}/linker.ld")
  set (_board_defines LED_PIN=${LED_PIN})
  set (_ncpu 4)
endif ()

set (_common_defines TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST ${_board_defines})

# Which of the shared tests this board can build at all.
#
# The SD and USB tests link micro-os-plus::devices, and its backends are
# written against the BCM2837's EMMC and DWC2 -- the RK3506 has neither at
# those addresses. Until those drivers grow a second backend, the Lyra
# builds the SoC-neutral subset: the tests that only need CPUs, a timer and
# a console. That is also the subset its predecessor project ever had.
set (_apps ${UOS_TEST_APPS})
if (BOARD STREQUAL "luckfox-lyra")
  foreach (_skip IN LISTS UOS_TEST_APPS_NEED_SD)
    list (REMOVE_ITEM _apps "${_skip}")
  endforeach ()
  list (REMOVE_ITEM _apps smp-mat-sdcard-test usb_test)
endif ()

foreach (_app IN LISTS _apps)
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
