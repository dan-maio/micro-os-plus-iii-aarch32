#!/usr/bin/env bash
# qemu.sh — run a µOS++ RPi-Zero-2W test under QEMU raspi2b.
#
# QEMU only ships a 32-bit quad-A7 Pi model (raspi2b, BCM2836): same
# 0x3F000000 peripheral block and 0x40000000 ARM-local-peripheral layout as the
# Pi Zero 2W's BCM2837, so it validates the whole board layer (UART, MMU, the
# per-core generic-timer tick, the mailbox IPI, and 4-core SMP bring-up).
#
# IMPORTANT: raspi2b is a Cortex-A7 (ARMv7-A) and CANNOT execute the ARMv8
# load-acquire/store-release atomics (lda/stl) GCC emits for -mcpu=cortex-a53.
# So the test is built here for QEMU with  make CPU=cortex-a7 FPU=neon-vfpv4
# (real hardware uses the default cortex-a53 build — see each test's hw.sh).
#
# SEMIHOST is enabled so the console is mirrored to QEMU (serial console is
# disabled to avoid duplicated output) and each test's semihosted SYS_EXIT
# ends the run with a RESULT (no infinite idle).
#
# Usage: ./qemu.sh <test-dir>            e.g. ./qemu.sh smp_test0
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${1:-smp_test0}"
cd "$HERE/$DIR"
make CPU=cortex-a7 FPU=neon-vfpv4 SEMIHOST=1
QEMU="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-arm 2>/dev/null | tail -1)"
[[ -x "$QEMU" ]] || QEMU=qemu-system-arm
exec "$QEMU" -M raspi2b -cpu cortex-a7 -nographic -serial none \
     -semihosting-config enable=on,target=native -kernel output/kernel7.img
