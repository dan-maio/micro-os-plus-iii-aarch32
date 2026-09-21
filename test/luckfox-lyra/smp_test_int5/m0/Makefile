# Makefile — RK3506 Cortex-M0 firmware for smp_test_int5.
# Produces m0_fw.elf / .bin / .lst. The .bin is loaded at the M0 boot address
# (by the A7, or via openocd_m0.cfg). Uses the same arm-none-eabi toolchain,
# targeting cortex-m0.

TOOLCHAIN_PATH := $(shell ls -d $(HOME)/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/*/.content/bin 2>/dev/null | sort -V | tail -1)
ifneq ($(TOOLCHAIN_PATH),)
PREFIX := $(TOOLCHAIN_PATH)/arm-none-eabi-
else
PREFIX := arm-none-eabi-
endif

CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
OBJDUMP := $(PREFIX)objdump
SIZE    := $(PREFIX)size

CFLAGS  := -mcpu=cortex-m0 -mthumb -Os -ffreestanding -fno-builtin \
           -ffunction-sections -fdata-sections -Wall -Wextra -g3
LDFLAGS := -mcpu=cortex-m0 -mthumb -nostdlib -T linker.ld \
           -Wl,--gc-sections -Wl,-Map,m0_fw.map

SRCS := m0_start.c m0_main.c

all: m0_fw.bin m0_fw.lst

m0_fw.elf: $(SRCS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@
	$(SIZE) $@

m0_fw.bin: m0_fw.elf
	$(OBJCOPY) -O binary $< $@

m0_fw.lst: m0_fw.elf
	$(OBJDUMP) -d -S $< > $@

clean:
	rm -f m0_fw.elf m0_fw.bin m0_fw.lst m0_fw.map

.PHONY: all clean
