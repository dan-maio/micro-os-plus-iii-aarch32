# micro-os-plus-iii-aarch32 — integration (xpack-development) model. The kernel is
# added by the harness and consumed as micro-os-plus::iii-core (THIN; the port
# owns startup/trace); the SoC and device targets are the device packages'
# (micro-os-plus-iii-devices-*); the board-fact contract comes via
# micro-os-plus::test-support. Exports
# micro-os-plus::aarch32. (Authored for the integration — the smp-branch root
# used the standalone sibling + devices model.)
cmake_minimum_required (VERSION 3.20)
project (micro-os-plus-iii-aarch32 LANGUAGES C CXX ASM)

# -----------------------------------------------------------------------------
# The kernel (micro-os-plus::iii and its parts) is not added here. Like every
# µOS++ port, this is a leaf library: the consumer (the tests, an application)
# adds the kernel. The targets below link kernel targets by name, which CMake
# resolves when it generates, so the kernel may be added after the port.

set (UOS_AARCH32_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# The port reads no board. A board (its SoC folder, flags, defines, start-up and
# linker scripts) is the consumer's: the test platforms of micro-os-plus-iii
# build a `micro-os-plus::board` target and link it after this one.
if (NOT TARGET micro-os-plus::aarch32)
  add_library (micro-os-plus-iii-aarch32-interface INTERFACE)
  target_include_directories (micro-os-plus-iii-aarch32-interface INTERFACE "include")
  target_sources (micro-os-plus-iii-aarch32-interface INTERFACE
    src/context_switch.cpp src/exception_handler.cpp src/handlers.cpp src/semihosting-exit.cpp src/smp_secondary.cpp
    src/rtos/os-core.cpp)
  target_link_libraries (micro-os-plus-iii-aarch32-interface INTERFACE
    micro-os-plus::iii-core micro-os-plus::port-smp-decls micro-os-plus::test-support)
  add_library (micro-os-plus::aarch32 ALIAS micro-os-plus-iii-aarch32-interface)
  message (STATUS "µOS++ IIIe aarch32 port: micro-os-plus::aarch32")
endif ()

# -----------------------------------------------------------------------------
# The SoC and device targets (micro-os-plus::soc-*, ::devices*) are the device
# packages': micro-os-plus-iii-devices-bcm283x, -rk3506 and -storage.
