# -----------------------------------------------------------------------------
# Luckfox Lyra B — RK3506, 3x Cortex-A7 (+ a Cortex-M0 that is not one of them).
#
# Three CPUs, not four. The RK3506 also carries a Cortex-M0, but it is a
# different ISA outside the A7 coherency domain: it cannot take an IPI as a
# peer and the scheduler cannot place a thread on it. The board is multi-core;
# the SMP cluster is the three identical A7s.
# -----------------------------------------------------------------------------

# This board's sources are its own.
set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")

set (UOS_BOARD_FLAGS -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4)
set (UOS_BOARD_NCPU  3)

# CAPS is what THIS PORT CAN DRIVE on this board.
#
# `sdcard` is here because the predecessor's cortex-a7 project drove this
# board's microSD -- src/sdcard/sdmmc.cpp, a DesignWare MSHC driver at
# 0xFF480000 -- and that driver is now devices/soc/rk3506/. It is reached
# through micro-os-plus::devices-rk3506, which is the RK3506 SD host under the
# same flatfs/FatFs layers the Pi uses.
#
# `usb-device` is here because the predecessor's RK3506 DWC2 gadget stack --
# src/usb/ and include/usb/, 13 sources and 10 headers -- is now usb/ beside
# this file, and smp_test6, smp_test7, smp_test_int3 and smp_test_int4 drive
# it. It is NOT part of the port library; see the source sets below.
set (UOS_BOARD_CAPS  smp sdcard usb-device led)

# One linker script, because there is one place this board runs.
#
# There is deliberately no UOS_BOARD_LINKER_QEMU. QEMU models none of the
# RK3506's own blocks -- the CRU, the GIC-400 at 0xFF58_0000, the DesignWare
# SD host, the DWC2 gadget, the SRAM mailbox, the Cortex-M0 -- so an emulated
# Lyra would be a generic Cortex-A7 wearing the name, and every test that
# makes this board worth having would be exactly the part not modelled.
#
# test/CMakeLists.txt reads the silence: a board with no QEMU linker script
# gets one image per test, `hwd`, and no emulated suite. This board is tested
# on hardware.
set (UOS_BOARD_LINKER_HW "${UOS_BOARD_SRC_DIR}/linker.ld")

set (UOS_BOARD_LIBS  "")

# The driver set an application links when it needs this board's SD card or
# USB: the RK3506 SD host under the same flatfs/FatFs layers the Pi uses.
set (UOS_BOARD_DEVICES  micro-os-plus::devices-rk3506)

# These Cortex-A7 are real OpenOCD `cortex_a` targets, which expect the
# classic AArch32 Angel trap, SVC 0x123456. HLT #0xF000 has no encoding on
# ARMv7-A -- see board-contract.cpp.
# A real GIC-400 SGI. SGI 0 is the lowest software-generated interrupt
     # and is unused by the miniloader.
set (UOS_BOARD_DEFINES
     OS_SMP_IPI_SGI=0
     SOC_RK3506
     SEMIHOST_TRAP_CHOSEN
     "PORT_GREETING=\"µOS++ Luckfox Lyra B (RK3506, Cortex-A7 SMP)\"")

# -----------------------------------------------------------------------------
# Code this board carries FOR ITS TESTS, compiled only by the tests that ask.
#
# None of it goes into the port library, and that is not an optimisation. The
# predecessor's per-test Makefiles each listed exactly which of these files
# they compiled, because several tests ship their OWN copy of one of them:
# smp_test6 and smp_test7 each have a different usb_vendor_gadget.cpp,
# smp_test_int4 a different sdmmc.cpp. Compiling the shared copy into every
# application would be a duplicate definition of the symbols the test means to
# provide itself. test/luckfox-lyra/tests.cmake names, per test, what it takes
# -- the same list the predecessor's Makefile did.

# The RK3506 DWC2 device (gadget) stack, carried over whole from
# smp-old/.../{include,src}/usb/.
#
# USB_CORE is the eight sources the predecessor's smp_test6 and smp_test7
# Makefiles compiled from src/usb/. The other five are in usb/src/ because
# they are part of the stack as it was written -- board_init.cpp and
# usb1_dev.cpp belong to the predecessor's own demo, and dma_pool.cpp,
# usb_env_stateos.cpp and usb_vendor_gadget.cpp are the three every test
# replaces -- not because anything here builds them.
#
# smp_test_int3 and smp_test_int4 take none of the sources, only the headers:
# their usb/usb1_int3.cpp drives the DWC2 registers directly.
set (UOS_BOARD_USB_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/usb/include")
set (UOS_BOARD_USB_CORE_SOURCES
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/udc_core.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/usb_composite.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/usb_config.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/usb_endpoint_autoconfig.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/usb_string_table.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/dwc2_driver.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/dwc2_ep0.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/dwc2_gadget.cpp")

# Two more the board owns, kept out of USB_CORE_SOURCES because only the
# gadget tests want them: smp_test6 and smp_test7 each carried a
# BYTE-IDENTICAL private copy of both until the duplicate gate named them.
set (UOS_BOARD_USB_GADGET_SOURCES
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/dma_pool.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/usb_env_stateos.cpp")

# The register-level USB driver the two interrupt tests drive directly,
# instead of the core sources above. smp_test_int3 and smp_test_int4 carried
# a byte-identical copy each; it now lives with the board, like everything
# else both of them use.
set (UOS_BOARD_USB_INT_SOURCES
     "${CMAKE_CURRENT_LIST_DIR}/usb/src/usb1_int3.cpp")

# FatFs R0.15a compiled as C++ inside `namespace fatfs`, carried over from
# smp-old/.../src/sdcard/fatfs_cpp/.
#
# This is a SECOND FatFs in the workspace: micro-os-plus-iii-devices/fatfs/ is
# the same R0.15a as C, and the two are not interchangeable. The C++ one wraps
# Dir and File in RAII classes and returns `enum class` results, which is the
# API smp_test5, smp_test7 and smp_test_int4 were written against. The C one
# is what flatfs and the Pi's sd_test use. Neither can serve the other, and
# they do not collide: every symbol of this one is inside `namespace fatfs`.
#
# DISKIO is separate from SOURCES because smp_test_int4 supplies its own --
# sd/diskio_sd_irq.cpp, over its own interrupt-driven sdmmc.cpp.
set (UOS_BOARD_FATFS_CPP_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/fatfs-cpp")
set (UOS_BOARD_FATFS_CPP_SOURCES
     "${CMAKE_CURRENT_LIST_DIR}/fatfs-cpp/ff.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/fatfs-cpp/ffsystem.cpp"
     "${CMAKE_CURRENT_LIST_DIR}/fatfs-cpp/ffunicode.cpp")
set (UOS_BOARD_FATFS_CPP_DISKIO
     "${CMAKE_CURRENT_LIST_DIR}/fatfs-cpp/diskio_sdmmc.cpp")
