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
# `usb-device` is still absent: the predecessor's RK3506 DWC2 gadget stack
# (13 files under src/usb/) has not been carried over yet.
set (UOS_BOARD_CAPS  smp sdcard led)

# The emulated runs use QEMU's `virt` machine -- generic Cortex-A7, DRAM at
# 0x4000_0000 -- because none of the RK3506's own blocks are modelled. The
# hardware runs link for the Lyra's own map.
set (UOS_BOARD_LINKER_HW   "${UOS_BOARD_SRC_DIR}/linker.ld")
set (UOS_BOARD_LINKER_QEMU "${UOS_BOARD_SRC_DIR}/linker-qemu-virt.ld")

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
