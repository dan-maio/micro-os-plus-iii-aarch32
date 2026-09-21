# -----------------------------------------------------------------------------
# micro-os-plus-iii-aarch32 -- ARMv7-A (32-bit) SMP port of µOS++ III.
#
# An independent project. It consumes the kernel and the device drivers;
# neither of them knows this project exists, so the dependency runs one way
# and the kernel tree stays merge-clean against upstream.
#
# It holds no copy of them. Clone the two repositories beside this one:
#
#   git clone .../micro-os-plus-iii-smp.git
#   git clone .../micro-os-plus-iii-devices.git
#   git clone .../micro-os-plus-iii-ARCH.git
#
# and they are found automatically as siblings. One working copy of the
# kernel serves every architecture project on the machine, so an edit to it
# is visible to all of them at once -- which is the point.
#
# Boards are build configurations of this one port, not separate ports:
# Pi Zero 2 W and Pi 3 B differ by a linker script and a banner macro.
#
# The BCM2837's Cortex-A53 cores run here in AArch32 (ARMv7-A), which is what
# lets this same port also serve the Cortex-A7 SoCs. Pick one with
# Pick one with -DBOARD=zero2w (default), rpi3b or luckfox-lyra.
#
# Exports: micro-os-plus::aarch32
# -----------------------------------------------------------------------------
cmake_minimum_required (VERSION 3.20)

project (micro-os-plus-iii-aarch32 LANGUAGES C CXX ASM)

set (UOS_AARCH32_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# -----------------------------------------------------------------------------
# Dependencies: sibling clones by default. Override either with -D to build
# against a working copy somewhere else.
get_filename_component (_uos_siblings "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set (UOS_SMP_DIR "${_uos_siblings}/micro-os-plus-iii-smp"
     CACHE PATH "µOS++ III SMP kernel working copy")
set (UOS_DEVICES_DIR "${_uos_siblings}/micro-os-plus-iii-devices"
     CACHE PATH "µOS++ III devices working copy")

foreach (_dep_var IN ITEMS UOS_SMP_DIR UOS_DEVICES_DIR)
  if (NOT EXISTS "${${_dep_var}}/CMakeLists.txt")
    string (REPLACE "UOS_" "" _repo "${_dep_var}")
    string (REPLACE "_DIR" "" _repo "${_repo}")
    string (TOLOWER "${_repo}" _repo)
    message (FATAL_ERROR
      "Cannot find the ${_repo} repository at ${${_dep_var}}.\n"
      "This project keeps no copy of it. Clone it beside this one:\n"
      "    git clone <remote>/micro-os-plus-iii-${_repo}.git\n"
      "or point at an existing working copy:\n"
      "    cmake -D${_dep_var}=/path/to/it ...")
  endif ()
endforeach ()

add_subdirectory ("${UOS_SMP_DIR}" micro-os-plus-iii-smp)
add_subdirectory ("${UOS_DEVICES_DIR}" micro-os-plus-iii-devices)

# -----------------------------------------------------------------------------
# Which board. This port is ARMv7-A, and ARMv7-A is not one machine: the same
# instruction set is reached from a Broadcom BCM2837 running in 32-bit mode
# and from a Rockchip RK3506. Everything that differs between them -- startup,
# the page tables, the interrupt controller, the tick, how secondary cores are
# released, the UART -- lives under boards/<dir>/. What does not differ stays
# in src/ and include/ and is compiled once for either.
#
# A core name is not an axis of its own: cortex-a53 and cortex-a7 are -mcpu
# flags a board chooses.
#
# Nor is the core COUNT, so it is not set here -- uos_add_app takes NCPU and
# defines OS_NCPU:
#
#   zero2w / rpi3b    4x Cortex-A53      OS_NCPU=4
#   luckfox-lyra      3x Cortex-A7       OS_NCPU=3
#
# The Lyra also carries a Cortex-M0, and it is deliberately not counted. SMP
# needs identical, cache-coherent peers: the three A7s are identical, so they
# are the SMP domain. The M0 is not -- different ISA, outside the coherency
# domain -- so it can be neither scheduled on nor sent an IPI as a peer. The
# board is multi-CPU; only part of it is symmetric. The M0 is an AMP companion
# reached through the hardware mailbox, which is why it appears solely as an
# interrupt SOURCE, in smp_test_int5.
set (BOARD "zero2w" CACHE STRING "Target board: zero2w, rpi3b or luckfox-lyra")
set_property (CACHE BOARD PROPERTY STRINGS zero2w rpi3b luckfox-lyra)

# zero2w and rpi3b are the same silicon and the same port sources; they differ
# by a linker script and a banner, so they share one board directory.
#
# SEMIHOST_TRAP_HLT is about the DEBUGGER, not the CPU. OpenOCD's semihosting
# trap is whatever the target type it drives expects:
#
#   cortex_a (ARMv7-A)  SVC 0x123456   -- the classic AArch32 Angel trap
#   aarch64             HLT #0xF000    -- the ARMv8 encoding
#
# The Pi's Cortex-A53 are declared as `aarch64` targets even for this 32-bit
# port, because the ARMv7 `cortex_a` target does not support the -cti option
# the BCM2837 needs (see boards/rpi-zero-2w/bcm2837-aarch32.cfg). So the Pi
# wants HLT. The Lyra's Cortex-A7 are real `cortex_a` targets and want SVC --
# and on an ARMv7-A core the HLT word 0xE10F0070 is simply UNDEFINED, so
# getting this wrong is not a missing console, it is an undefined-instruction
# exception on the first character printed.
if (BOARD STREQUAL "luckfox-lyra")
  set (UOS_BOARD_DIR "luckfox-lyra")
  set (_board_flags -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4)
  set (_board_define SOC_RK3506
                     "PORT_GREETING=\"µOS++ Luckfox Lyra B (RK3506, Cortex-A7 SMP)\"")
  set (_board_libs "")
else ()
  set (UOS_BOARD_DIR "rpi-zero-2w")
  set (_board_flags -mcpu=cortex-a53 -marm -mfloat-abi=hard -mfpu=neon-fp-armv8)
  set (_board_define SOC_BCM2837 SEMIHOST_TRAP_HLT
                     "PORT_GREETING=\"µOS++ Raspberry Pi Zero 2W (BCM2837, Cortex-A53 AArch32 SMP)\"")
  set (_board_libs micro-os-plus::soc-bcm2837)
endif ()
set (UOS_BOARD_DIR "${UOS_BOARD_DIR}" CACHE INTERNAL "board source directory")

if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/boards/${UOS_BOARD_DIR}")
  message (FATAL_ERROR
    "BOARD=${BOARD} wants boards/${UOS_BOARD_DIR}/, which does not exist.")
endif ()

file (GLOB _board_sources CONFIGURE_DEPENDS
      "boards/${UOS_BOARD_DIR}/src/*.cpp" "boards/${UOS_BOARD_DIR}/src/*.S"
      "boards/${UOS_BOARD_DIR}/src/rtos/*.cpp")

# -----------------------------------------------------------------------------
# The port itself.
add_library (micro-os-plus-iii-aarch32-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-aarch32-interface
  INTERFACE "include" "boards/${UOS_BOARD_DIR}/include"
)

target_sources (
  micro-os-plus-iii-aarch32-interface
  INTERFACE
        # Shared ARMv7-A. exception_handler.cpp is the proof that this split
        # is real: its two SoC versions were byte-identical, 327 lines, same
        # checksum. handlers.cpp, context_switch.cpp and smp_secondary.cpp
        # differ by a dozen lines each and carry them as #ifdefs.
        #
        # src/rtos/os-core.cpp is the port's half of the scheduler. It used to
        # be a per-board file, and the two copies had drifted: the Pi's had
        # fixes -- the full d0-d31 VFP frame, the deferral that keeps a switch
        # from stranding the kernel spinlock, the release ordering in
        # switch_stacks() -- that the older copy did not. One copy cannot
        # drift. What genuinely differs, the interrupt controller, is
        # boards/<board>/src/rtos/port_isr.cpp.
        src/context_switch.cpp
        src/exception_handler.cpp
        src/handlers.cpp
        src/smp_secondary.cpp
        src/rtos/os-core.cpp
        # Per-board: startup, MMU tables, the tick, the IPI, and the port's half
        # of the scheduler. The kernel's own src/rtos/os-core.cpp is the
        # portable half and is compiled alongside it, exactly as the previous
        # Makefiles did.
        ${_board_sources}
)

target_compile_definitions (
  micro-os-plus-iii-aarch32-interface INTERFACE ${_board_define}
)

target_compile_options (
  micro-os-plus-iii-aarch32-interface
  # Only the silicon. The flags every bare-metal application shares
  # (-O2, -ffunction-sections, -fno-exceptions, -nostartfiles, ...) are
  # set once by uos_add_app.
  INTERFACE ${_board_flags}
)

# The same machine flags at link time: the driver has to pick the matching
# multilib, or the link fails on the float ABI.
target_link_options (
  micro-os-plus-iii-aarch32-interface
  INTERFACE ${_board_flags}
)

target_link_libraries (
  micro-os-plus-iii-aarch32-interface
  INTERFACE
        micro-os-plus::iii
        # os-decls.h: the C++ half of the SMP port contract, identical to the
        # AArch64 port's. This project supplies os-c-decls.h and os-inlines.h,
        # which are where the word size and the instructions actually live.
        micro-os-plus::port-smp-decls
        ${_board_libs}
)

add_library (micro-os-plus::aarch32 ALIAS micro-os-plus-iii-aarch32-interface)

# -----------------------------------------------------------------------------
if (PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory (test)
endif ()

message (STATUS "µOS++ III AArch32 port: micro-os-plus::aarch32, board ${BOARD}")
