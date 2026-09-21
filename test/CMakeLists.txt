# -----------------------------------------------------------------------------
# Test builds for the AArch32 port.
#
# Every board owns its tests. They live in boards/<id>/test/, one directory per
# application, and this file builds whatever is there -- twice, once per
# variant:
#
#   qemu  the build the QEMU suites run: semihosting on, no boot-card I/O
#   hwd   the build for real hardware: HW_BUILD, so the SD tests use the
#         existing FAT32 boot partition, plus the early-boot asm markers
#
# There is no shared test list and no capability filter, because there is
# nothing left to filter: a board builds the directories it has. A board with
# no SD card has no sd_test directory; a single-CPU board has no smp_* ones.
# Adding, removing or fixing a test touches one board and reaches no other.
#
# Each board's test/ also carries the support its tests share -- include/ and
# src/ -- and a tests.cmake naming the few knobs a directory listing cannot
# express: which tests link the board's driver set, and any per-test -D flags.
#
# BOARD selects the board; boards that share silicon share a test directory the
# same way they share src/ (rpi3b builds rpi-zero-2w's).
# -----------------------------------------------------------------------------

option (UOS_DEBUG_BOOT "Compile the hwd builds with the early-boot markers" OFF)

# UOS_BOARD_SRC_DIR is where the board's CODE is, and two boards may share it:
# rpi3b compiles rpi-zero-2w's src/ and include/ with -DBOARD_RPI3B, exactly as
# the predecessor's `make BOARD=rpi3b` did.
#
# The TESTS are not shared that way. Every board has its own test/ directory,
# under the board, so changing or adding a test reaches one board and no other.
# That is why this is boards/${BOARD}/test and not ${UOS_BOARD_SRC_DIR}/test.
set (_boards_dir "${UOS_BOARD_SRC_DIR}")
get_filename_component (_test_dir
  "${CMAKE_CURRENT_SOURCE_DIR}/../boards/${BOARD}/test" ABSOLUTE)

if (NOT IS_DIRECTORY "${_test_dir}")
  message (FATAL_ERROR
           "boards/${BOARD} has no test/ directory. Every board has one -- it "
           "is where that board's test applications live, even when the board "
           "shares its src/ and include/ with another.")
endif ()

set (_linker_hwd  "${UOS_BOARD_LINKER_HW}")
set (_linker_qemu "${UOS_BOARD_LINKER_QEMU}")
set (_ncpu        "${UOS_BOARD_NCPU}")

set (_common_defines TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST)

# The knobs a directory listing cannot express. Optional: a board whose tests
# need nothing beyond the defaults has no tests.cmake.
set (BOARD_TEST_NEED_DEVICES "")
# Tests that bring their own start-up code and must NOT also get test/src/.
set (BOARD_TEST_SELF_CONTAINED "")
function (board_test_defines _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
if (EXISTS "${_test_dir}/tests.cmake")
  include ("${_test_dir}/tests.cmake")
endif ()

# The support this board's tests share: one copy per board, not per test.
file (GLOB _test_support CONFIGURE_DEPENDS "${_test_dir}/src/*.cpp")
set (_test_includes "${_test_dir}/include")

# One application per directory. include/ and src/ are the support above, not
# applications, so they are skipped.
file (GLOB _test_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${_test_dir}/*")

set (_apps "")
foreach (_dir IN LISTS _test_dirs)
  if (NOT IS_DIRECTORY "${_dir}")
    continue ()
  endif ()
  get_filename_component (_app "${_dir}" NAME)
  if (_app STREQUAL "include" OR _app STREQUAL "src")
    continue ()
  endif ()
  file (GLOB _srcs CONFIGURE_DEPENDS "${_dir}/*.cpp")
  if (NOT _srcs)
    continue ()
  endif ()
  list (APPEND _apps "${_app}")

  board_test_defines ("${_app}" _app_defines)

  # A test carried over whole from the predecessor defines its own
  # smp_install_boot_threads(); adding the board's shared copy would be a
  # duplicate definition, and picking one would mean editing a test that
  # already passed on hardware.
  set (_app_support ${_test_support})
  if ("${_app}" IN_LIST BOARD_TEST_SELF_CONTAINED)
    set (_app_support "")
  endif ()

  set (_libs micro-os-plus::aarch32)
  if ("${_app}" IN_LIST BOARD_TEST_NEED_DEVICES)
    list (APPEND _libs ${UOS_BOARD_DEVICES})
  endif ()

  foreach (_variant IN ITEMS qemu hwd)
    if (_variant STREQUAL "hwd")
      # DEBUG_BOOT is opt-in: every early-boot marker is a semihosting trap,
      # and under a JTAG probe each one costs real time. Turn it on only when
      # bringing a board up.
      set (_variant_defines HW_BUILD)
      if (UOS_DEBUG_BOOT)
        list (APPEND _variant_defines DEBUG_BOOT)
      endif ()
      set (_variant_linker "${_linker_hwd}")
    else ()
      # QEMU_BUILD says "this image runs under the emulator", which is not the
      # same as "not hardware": a board whose silicon QEMU does not model
      # builds its emulated image against the `virt` machine instead.
      set (_variant_defines QEMU_BUILD)
      set (_variant_linker "${_linker_qemu}")
    endif ()

    uos_add_app (
      "${_app}-${_variant}"
      SOURCES ${_srcs} ${_app_support}
      NCPU ${_ncpu}
      LINKER_SCRIPT "${_variant_linker}"
      DEFINES ${_common_defines} ${_app_defines} ${_variant_defines}
      INCLUDES ${_test_includes}
      LIBRARIES ${_libs}
      OUTPUT_NAME "${_app}-${_variant}"
    )
  endforeach ()
endforeach ()

list (LENGTH _apps _n)
math (EXPR _n "${_n} * 2")
message (STATUS "µOS++ III AArch32 tests: ${_n} targets, board=${BOARD}, from ${_test_dir}")

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
