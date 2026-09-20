#!/usr/bin/env bash
# hw.sh — run the AArch32 hardware tests on a Raspberry Pi Zero 2W / Pi 3 B.
#
# This is only the ISA half: the session itself is driven by the shared
# runner in the kernel repository (test/run-hw.sh), which every port uses.
# Everything AArch32-specific is here, and it is four facts:
#
#   * arm-none-eabi binutils read the ELF;
#   * __smp_spin is uint32_t[4], so four 32-bit words are zeroed;
#   * resume means "force CPSR to 0x600001da (SVC, IRQ/FIQ masked) and resume
#     at the entry" -- unlike AArch64 the mode has to be set explicitly;
#   * the OpenOCD configs here end in `init; halt`, so the runner must NOT
#     issue a second init.
#
# On hardware nothing boots through the QEMU shim: config.txt sets
# arm_64bit=0 and the GPU starts the cores in AArch32 already.
#
# PURE OpenOCD: no GDB, no reset, and the UART console is NOT touched --
# keep your own terminal on /dev/ttyACM0.
#
# Usage:  test/hw.sh [app|all] [run-seconds]
# Env:    PROBE=jlink (default) | olimex
#         BUILD=<dir>  the CMake build directory (default: build)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
PROBE="${PROBE:-jlink}"

case "$PROBE" in
  jlink)  CFG="$ROOT/boards/rpi-zero-2w/openocd-jlink-rpi3.cfg" ;;
  olimex) CFG="$ROOT/boards/rpi-zero-2w/openocd-olimex.cfg" ;;
  *) echo "hw.sh: PROBE must be jlink or olimex, not '$PROBE'" >&2; exit 2 ;;
esac

# gcc 15 xPack binutils, as the builds use; PATH otherwise.
TC="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.*/.content/bin 2>/dev/null | sort -V | tail -1)"

SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"

UOS_HW_CFG="$CFG" \
UOS_HW_CFG_INIT=0 \
UOS_HW_NM="${TC:+$TC/}arm-none-eabi-nm" \
UOS_HW_READELF="${TC:+$TC/}arm-none-eabi-readelf" \
UOS_HW_ENTRY=0x1003c \
UOS_HW_SPIN_WORDS=4 \
UOS_HW_RESUME=cpsr \
UOS_HW_NCPU=4 \
exec "$SMP_DIR/test/run-hw.sh" "$BUILD/test" "${1:-all}" "${2:-}"
