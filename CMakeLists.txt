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
# Pick one with -DBOARD=rpi-zero-2w (default), rpi3b or luckfox-lyra.
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
# released, the UART -- lives under test/boards/<dir>/. What does not differ stays
# in src/ and include/ and is compiled once for either.
#
# A core name is not an axis of its own: cortex-a53 and cortex-a7 are -mcpu
# flags a board chooses.
#
# Nor is the core COUNT, so it is not set here -- uos_add_app takes NCPU and
# defines OS_NCPU:
#
#   rpi-zero-2w / rpi3b   4x Cortex-A53      OS_NCPU=4
#   luckfox-lyra      3x Cortex-A7       OS_NCPU=3
#
# The Lyra also carries a Cortex-M0, and it is deliberately not counted. SMP
# needs identical, cache-coherent peers: the three A7s are identical, so they
# are the SMP domain. The M0 is not -- different ISA, outside the coherency
# domain -- so it can be neither scheduled on nor sent an IPI as a peer. The
# board is multi-CPU; only part of it is symmetric. The M0 is an AMP companion
# reached through the hardware mailbox, which is why it appears solely as an
# interrupt SOURCE, in smp_test_int5.
# Boards are DISCOVERED, not listed. Every directory under test/boards/ that holds
# a board.cmake is a board, and that file is the only place its facts are
# written: sources, CPU flags, core count, capabilities, linker scripts,
# libraries and defines. Nothing here knows any board's name.
#
# A directory WITHOUT a board.cmake is not a board -- it is a source directory
# several boards share, the way rpi3b shares test/boards/rpi-zero-2w/.
# That is the shape a port with many boards on few SoCs needs.
file (GLOB _board_decls CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/boards/*/board.cmake")
set (_board_ids "")
foreach (_decl IN LISTS _board_decls)
  get_filename_component (_dir "${_decl}" DIRECTORY)
  get_filename_component (_id "${_dir}" NAME)
  list (APPEND _board_ids "${_id}")
endforeach ()
list (SORT _board_ids)

set (BOARD "rpi-zero-2w" CACHE STRING "Target board (one of: ${_board_ids})")
set_property (CACHE BOARD PROPERTY STRINGS ${_board_ids})

if (NOT BOARD IN_LIST _board_ids)
  message (FATAL_ERROR
    "BOARD=${BOARD} is not a board of this port.\n"
    "Available: ${_board_ids}\n"
    "A board is a directory under test/boards/ containing a board.cmake.")
endif ()

# Each board.cmake sets this contract. Anything a board leaves unset that the
# shared sources need becomes a compile error in src/board-contract.cpp, not a
# silent inheritance of another board's answer.
include ("${CMAKE_CURRENT_SOURCE_DIR}/test/boards/${BOARD}/board.cmake")

foreach (_required IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_FLAGS UOS_BOARD_NCPU
                            UOS_BOARD_CAPS UOS_BOARD_LINKER_HW
                            UOS_BOARD_LINKER_QEMU UOS_BOARD_DEFINES)
  if (NOT DEFINED ${_required})
    message (FATAL_ERROR
      "test/boards/${BOARD}/board.cmake does not set ${_required}.")
  endif ()
endforeach ()

get_filename_component (UOS_BOARD_SRC_DIR "${UOS_BOARD_SRC_DIR}" ABSOLUTE)
if (NOT IS_DIRECTORY "${UOS_BOARD_SRC_DIR}")
  message (FATAL_ERROR
    "test/boards/${BOARD}/board.cmake points at ${UOS_BOARD_SRC_DIR}, which does not exist.")
endif ()

# Published for test/CMakeLists.txt, which is a separate directory scope.
# A board that claims it can reach an SD card or a USB device must also say
# which driver set does it, or the capability is a promise nothing keeps.
foreach (_cap_lib IN ITEMS sdcard usb-device)
  if ("${_cap_lib}" IN_LIST UOS_BOARD_CAPS AND NOT DEFINED UOS_BOARD_DEVICES)
    message (FATAL_ERROR
             "test/boards/${BOARD}/board.cmake declares the `${_cap_lib}` capability "
             "but no UOS_BOARD_DEVICES to provide it.")
  endif ()
endforeach ()

# UOS_BOARD_CAPS is a statement about THIS PORT ON THIS BOARD -- the
# drivers it can actually bring up -- not an inventory of the board's
# connectors. A board with a microSD slot and no driver for it does not
# declare `sdcard`; the day the driver lands, the capability does too and
# the SD tests appear with no change to test/CMakeLists.txt.
foreach (_v IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_NCPU UOS_BOARD_CAPS
                     UOS_BOARD_LINKER_HW UOS_BOARD_LINKER_QEMU)
  set (${_v} "${${_v}}" CACHE INTERNAL "board fact")
endforeach ()

set (_board_flags  ${UOS_BOARD_FLAGS})
set (_board_define ${UOS_BOARD_DEFINES})
set (_board_libs   ${UOS_BOARD_LIBS})

file (GLOB _board_sources CONFIGURE_DEPENDS
      "${UOS_BOARD_SRC_DIR}/src/*.cpp" "${UOS_BOARD_SRC_DIR}/src/*.S"
      "${UOS_BOARD_SRC_DIR}/src/rtos/*.cpp")

# -----------------------------------------------------------------------------
# The port itself.
add_library (micro-os-plus-iii-aarch32-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-aarch32-interface
  INTERFACE "include" "${UOS_BOARD_SRC_DIR}/include"
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
        # test/boards/<board>/src/rtos/port_isr.cpp.
        src/context_switch.cpp
        src/exception_handler.cpp
        src/handlers.cpp
        # Strong _Exit(): terminate through semihosting, so std::exit() ends
        # the run with a real exit code instead of idling.
        src/semihosting-exit.cpp
        src/smp_secondary.cpp
        src/rtos/os-core.cpp
        # No code. Fails the build the moment a board has not stated
        # something the shared sources need, instead of letting it inherit
        # the previous board's answer. See the file's own header.
        src/board-contract.cpp
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
