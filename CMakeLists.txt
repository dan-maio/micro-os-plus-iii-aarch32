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
# -DUOS_SOC=bcm2837 (default) or -DUOS_SOC=rk3506.
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
# Which silicon. This port is ARMv7-A, and ARMv7-A is not one SoC: the same
# instruction set is reached from a Broadcom BCM2837 running in 32-bit mode and
# from a Rockchip RK3506. What differs between them -- the interrupt
# controller, the timer, the way secondary cores are released, the UART, the
# page tables -- lives under soc/<name>/. What does not differ stays in src/
# and include/, compiled once for either.
set (UOS_SOC "bcm2837" CACHE STRING "Target SoC: bcm2837 or rk3506")
set_property (CACHE UOS_SOC PROPERTY STRINGS bcm2837 rk3506)

if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/soc/${UOS_SOC}")
  message (FATAL_ERROR
    "UOS_SOC=${UOS_SOC} has no soc/${UOS_SOC}/ directory in this port.")
endif ()

# Per-SoC facts the shared code cannot infer. -mcpu differs because the two
# parts are different cores that happen to share an instruction set; SOC_* is
# what the few #ifdefs in src/ test.
if (UOS_SOC STREQUAL "rk3506")
  set (_soc_flags -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4)
  set (_soc_define SOC_RK3506)
  set (_soc_libs "")
else ()
  set (_soc_flags -mcpu=cortex-a53 -marm -mfloat-abi=hard -mfpu=neon-fp-armv8)
  set (_soc_define SOC_BCM2837)
  set (_soc_libs micro-os-plus::soc-bcm2837)
endif ()

file (GLOB _soc_sources CONFIGURE_DEPENDS
      "soc/${UOS_SOC}/src/*.cpp" "soc/${UOS_SOC}/src/*.S"
      "soc/${UOS_SOC}/src/rtos/*.cpp")

# -----------------------------------------------------------------------------
# The port itself.
add_library (micro-os-plus-iii-aarch32-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-aarch32-interface
  INTERFACE "include" "soc/${UOS_SOC}/include"
)

target_sources (
  micro-os-plus-iii-aarch32-interface
  INTERFACE
        # Shared ARMv7-A. exception_handler.cpp is the proof that this split
        # is real: its two SoC versions were byte-identical, 327 lines, same
        # checksum. handlers.cpp, context_switch.cpp and smp_secondary.cpp
        # differ by a dozen lines each and carry them as #ifdefs.
        src/context_switch.cpp
        src/exception_handler.cpp
        src/handlers.cpp
        src/smp_secondary.cpp
        # Per-SoC: startup, MMU tables, the tick, the IPI, and the port's half
        # of the scheduler. The kernel's own src/rtos/os-core.cpp is the
        # portable half and is compiled alongside it, exactly as the previous
        # Makefiles did.
        ${_soc_sources}
)

target_compile_definitions (
  micro-os-plus-iii-aarch32-interface INTERFACE ${_soc_define}
)

target_compile_options (
  micro-os-plus-iii-aarch32-interface
  # Only the silicon. The flags every bare-metal application shares
  # (-O2, -ffunction-sections, -fno-exceptions, -nostartfiles, ...) are
  # set once by uos_add_app.
  INTERFACE ${_soc_flags}
)

# The same machine flags at link time: the driver has to pick the matching
# multilib, or the link fails on the float ABI.
target_link_options (
  micro-os-plus-iii-aarch32-interface
  INTERFACE ${_soc_flags}
)

target_link_libraries (
  micro-os-plus-iii-aarch32-interface
  INTERFACE
        micro-os-plus::iii
        # os-decls.h: the C++ half of the SMP port contract, identical to the
        # AArch64 port's. This project supplies os-c-decls.h and os-inlines.h,
        # which are where the word size and the instructions actually live.
        micro-os-plus::port-smp-decls
        ${_soc_libs}
)

add_library (micro-os-plus::aarch32 ALIAS micro-os-plus-iii-aarch32-interface)

# -----------------------------------------------------------------------------
if (PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory (test)
endif ()

message (STATUS "µOS++ III AArch32 port: micro-os-plus::aarch32, SoC ${UOS_SOC}")
