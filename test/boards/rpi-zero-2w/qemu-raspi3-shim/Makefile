# Build the AArch64->AArch32 bootstrap shim for running the 32-bit µOS++
# kernel under QEMU's 64-bit raspi3b machine (real Cortex-A53). Hardware does
# NOT use this (config.txt arm_64bit=0 boots AArch32 directly).
A64_PATH := $(shell ls -d $(HOME)/.local/xPacks/@xpack-dev-tools/aarch64-none-elf-gcc/*/.content/bin 2>/dev/null | sort -V | tail -1)
ifneq ($(A64_PATH),)
PREFIX := $(A64_PATH)/aarch64-none-elf-
else
PREFIX := aarch64-none-elf-
endif

all: shim8.img

shim.elf: shim.S shim.ld
	$(PREFIX)gcc -nostdlib -Wl,-T,shim.ld $< -o $@

shim8.img: shim.elf
	$(PREFIX)objcopy -O binary $< $@
	@echo "Created $@"

clean:
	rm -f shim.elf shim8.img

.PHONY: all clean
