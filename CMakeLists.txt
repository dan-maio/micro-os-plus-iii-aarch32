# micro-os-plus-iii-aarch32 — integration (xpack-development) model. The kernel is
# added by the harness and consumed as micro-os-plus::iii-core (THIN; the port
# owns startup/trace); the dissolved SoC targets are defined here (their
# sources are compiled through the consumer's micro-os-plus::board); the
# board-fact contract comes via micro-os-plus::test-support. Exports
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
# Dissolved devices support: SD host, USB, and FatFs stack.
if (NOT TARGET micro-os-plus::devices)
  add_library (micro-os-plus-iii-devices-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-devices-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs"
  )
  target_sources (
    micro-os-plus-iii-devices-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/flatfs.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/usb_dwc2.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/ff.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/diskio_sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/fatfs_hw.cpp"
  )
  add_library (micro-os-plus::devices ALIAS micro-os-plus-iii-devices-interface)
endif ()

# -----------------------------------------------------------------------------
# Dissolved SoC support: BCM2837 silicon (formerly micro-os-plus-iii-devices).
# Headers only: the board target (micro-os-plus::board, built by the consumer)
# compiles soc/<soc>/src/*.cpp, so listing them here as well would link them
# twice.
if (NOT TARGET micro-os-plus::soc-bcm2837)
  add_library (micro-os-plus-iii-soc-bcm2837-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-soc-bcm2837-interface
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/soc/bcm2837/include"
  )
  add_library (micro-os-plus::soc-bcm2837
               ALIAS micro-os-plus-iii-soc-bcm2837-interface)
endif ()

# -----------------------------------------------------------------------------
# Dissolved devices support over the RK3506 SD host (Luckfox Lyra B): the same
# filesystem drivers as micro-os-plus::devices, the DesignWare SD backend, no
# USB gadget.
if (NOT TARGET micro-os-plus::devices-rk3506)
  add_library (micro-os-plus-iii-devices-rk3506-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-devices-rk3506-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs"
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/rk3506/include"
  )
  target_compile_definitions (
    micro-os-plus-iii-devices-rk3506-interface INTERFACE SD_BACKEND_DWMMC
  )
  target_sources (
    micro-os-plus-iii-devices-rk3506-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/rk3506/src/sdmmc.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/rk3506/src/sd_dwmmc.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/flatfs.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/ff.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/diskio_sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/fatfs_hw.cpp"
  )
  add_library (micro-os-plus::devices-rk3506
               ALIAS micro-os-plus-iii-devices-rk3506-interface)
endif ()
