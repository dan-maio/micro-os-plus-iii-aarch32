# -----------------------------------------------------------------------------
# Raspberry Pi 3 B — BCM2837, 4x Cortex-A53 running in AArch32.
#
# Same silicon and same sources as rpi-zero-2w; a different linker script, and an
# ACT LED that is not a SoC GPIO at all but VideoCore expander GPIO 130,
# driven through the mailbox -- so no LED_PIN.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../rpi-zero-2w")

set (UOS_BOARD_FLAGS -mcpu=cortex-a53 -marm -mfloat-abi=hard -mfpu=neon-fp-armv8)
set (UOS_BOARD_NCPU  4)
set (UOS_BOARD_CAPS  smp sdcard usb-device led)

set (UOS_BOARD_LINKER_HW   "${UOS_BOARD_SRC_DIR}/linker-rpi3b.ld")
set (UOS_BOARD_LINKER_QEMU "${UOS_BOARD_SRC_DIR}/linker-rpi3b.ld")

set (UOS_BOARD_LIBS  micro-os-plus::soc-bcm2837)

# The driver set an application links when it needs the SD card or USB:
# the BCM2837 SD host and DWC2, which is what these tests always linked.
set (UOS_BOARD_DEVICES  micro-os-plus::devices)

# BCM2837 has no GIC and no SGIs: a cross-core reschedule is a write to
     # the target core's local mailbox 0.
set (UOS_BOARD_DEFINES
     OS_SMP_IPI_SGI=0
     SOC_BCM2837 BOARD_RPI3B
     SEMIHOST_TRAP_CHOSEN SEMIHOST_TRAP_HLT
     "PORT_GREETING=\"µOS++ Raspberry Pi 3 B (BCM2837, Cortex-A53 AArch32 SMP)\"")
