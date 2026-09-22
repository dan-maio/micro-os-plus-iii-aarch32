# -----------------------------------------------------------------------------
# Test builds for the AArch32 port.
#
# Every board owns its tests. They live in test/<board>/, one directory per
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
# The TESTS are never shared that way. Each board has its own directory here --
# test/<board>/ -- so changing or adding a test reaches one board and no other.
# That is why this is test/${BOARD} and not ${UOS_BOARD_SRC_DIR}/test.
set (_boards_dir "${UOS_BOARD_SRC_DIR}")
set (_test_dir   "${CMAKE_CURRENT_SOURCE_DIR}/${BOARD}")

if (NOT IS_DIRECTORY "${_test_dir}")
  message (FATAL_ERROR
           "test/${BOARD} does not exist. Every board has a directory here -- "
           "it is where that board's test applications live, even when the "
           "board shares its src/ and include/ with another.")
endif ()

set (_linker_hwd  "${UOS_BOARD_LINKER_HW}")
set (_linker_qemu "${UOS_BOARD_LINKER_QEMU}")

# One image per test, or two.
#
# Most boards get both: `qemu` for the emulated suite and `hwd` for the probe.
# A board whose board.cmake sets no UOS_BOARD_LINKER_QEMU gets only `hwd`,
# because there is nowhere for the other image to run.
#
# That is not a limitation of QEMU's ARM support -- it is what the board IS.
# The Luckfox Lyra is the case: QEMU models none of the RK3506's own blocks,
# so an emulated Lyra would be a generic Cortex-A7 wearing the name, and
# every test that makes the board worth having -- the DWC2 gadget, the SD
# host, the Cortex-M0 mailbox -- would be exactly the part not emulated. The
# board is tested on hardware, and says so by staying silent here rather than
# by carrying a second linker script and a suite that cannot pass.
if (_linker_qemu)
  set (_variants qemu hwd)
else ()
  set (_variants hwd)
endif ()

set (_common_defines TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST)

# The knobs a directory listing cannot express. Optional: a board whose tests
# need nothing beyond the defaults has no tests.cmake.
set (BOARD_TEST_NEED_DEVICES "")
# Tests that bring their own start-up code and must NOT also get test/src/.
set (BOARD_TEST_SELF_CONTAINED "")
function (board_test_defines _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
# Sources and include directories ONE test needs beyond its own directory.
#
# A test carried over from the predecessor may bring a subdirectory of its own
# -- usb/, sd/, m0/ -- and may take part of the code the board carries for its
# tests. It cannot be a glob: several of those tests ship their own copy of a
# file the board also has, so which copy a test compiles is a decision, not a
# directory listing. The board's tests.cmake makes it, one test at a time,
# exactly as that test's Makefile did.
function (board_test_sources _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
function (board_test_includes _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
# The LINKER SCRIPT one test needs instead of the board's.
#
# A board is one piece of hardware, so one memory map -- until a test changes
# where it runs. On the RP2350B the PSRAM tests link their data into the
# external PSRAM at 0x11000000 while every other test on the same board keeps
# its data in internal SRAM, and the predecessor said so with one LDSCRIPT
# line per Makefile. Returning nothing keeps the board's own script, which is
# what every test of every other board does.
function (board_test_linker _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
# COMPILE OPTIONS one test needs on ITS OWN sources.
#
# Not on the kernel it links: these are options that belong to the way a
# particular application is written. The RP2350's nested-interrupt tests are
# the case -- they place their handlers in .data so the code runs from RAM
# rather than XIP flash, and the predecessor's Makefiles gave exactly those
# main.cpp files "-g0 -mlong-calls", with the comment "disabling debug symbols
# and enabling long-calls for RAM function support". Both halves are load
# bearing: a call from flash to a .data function is out of a Thumb BL's reach,
# and GCC's location views cannot describe a function whose section moved
# ("Error: leb128 operand is an undefined symbol").
function (board_test_options _app _out)
  set (${_out} "" PARENT_SCOPE)
endfunction ()
# How many CPUs ONE test runs on, when that is not the whole board.
#
# NCPU is a board fact for every board of these ports but one: the RP2350 has
# two cores, and two of its tests are deliberately single-core -- smp-test1 is
# the single-core RTOS bring-up and sc-test-ko is the single-core twin of
# smp-test-ko, the only test on that silicon exercising the kernel's NON-SMP
# branch. Their Makefiles simply did not define OS_USE_SMP_SCHEDULER.
# uos_add_app() derives that define from NCPU, so returning 1 is the whole of
# it.
function (board_test_ncpu _app _out)
  set (${_out} "${UOS_BOARD_NCPU}" PARENT_SCOPE)
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

  board_test_defines  ("${_app}" _app_defines)
  board_test_sources  ("${_app}" _app_sources)
  board_test_includes ("${_app}" _app_includes)
  board_test_linker   ("${_app}" _app_linker)
  board_test_options  ("${_app}" _app_options)
  board_test_ncpu     ("${_app}" _app_ncpu)

  # A test carried over whole from the predecessor defines its own
  # smp_install_boot_threads(); adding the board's shared copy would be a
  # duplicate definition, and picking one would mean editing a test that
  # already passed on hardware.
  set (_app_support ${_test_support})
  if ("${_app}" IN_LIST BOARD_TEST_SELF_CONTAINED)
    set (_app_support "")
  endif ()

  # The application's own sources carry its own options, if it has any --
  # every other source of the target keeps the build's.
  if (_app_options)
    set_source_files_properties (${_srcs} PROPERTIES COMPILE_OPTIONS "${_app_options}")
  endif ()

  set (_libs micro-os-plus::aarch32)
  if ("${_app}" IN_LIST BOARD_TEST_NEED_DEVICES)
    list (APPEND _libs ${UOS_BOARD_DEVICES})
  endif ()

  foreach (_variant IN LISTS _variants)
    if (_variant STREQUAL "hwd")
      # DEBUG_BOOT is opt-in: every early-boot marker is a semihosting trap,
      # and under a JTAG probe each one costs real time. Turn it on only when
      # bringing a board up.
      set (_variant_defines HW_BUILD)
      if (UOS_DEBUG_BOOT)
        list (APPEND _variant_defines DEBUG_BOOT)
      endif ()
      set (_variant_linker "${_linker_hwd}")
      # A test that names its own script takes it. Only the hardware image:
      # an emulated machine has the memory map QEMU gives it, not the one a
      # board's external memory happens to have.
      if (_app_linker)
        set (_variant_linker "${_app_linker}")
      endif ()
    else ()
      # QEMU_BUILD says "this image runs under the emulator", which is not the
      # same as "not hardware": a board whose silicon QEMU does not model
      # builds its emulated image against the `virt` machine instead.
      set (_variant_defines QEMU_BUILD)
      set (_variant_linker "${_linker_qemu}")
    endif ()

    uos_add_app (
      "${_app}-${_variant}"
      SOURCES ${_srcs} ${_app_support} ${_app_sources}
      NCPU ${_app_ncpu}
      LINKER_SCRIPT "${_variant_linker}"
      DEFINES ${_common_defines} ${_app_defines} ${_variant_defines}
      # ${_dir} is the application's own directory, which the predecessor's
      # Makefiles all put on the include path as -I. -- an application whose
      # headers sit beside its sources and includes them as <foo.hpp> needs
      # it, and one that writes "foo.hpp" never notices. Each application is
      # its own target, so this reaches no other.
      INCLUDES ${_app_includes} ${_dir} ${_test_includes}
      LIBRARIES ${_libs}
      OUTPUT_NAME "${_app}-${_variant}"
    )
  endforeach ()
endforeach ()

list (LENGTH _apps _n)
list (LENGTH _variants _nv)
math (EXPR _n "${_n} * ${_nv}")
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
