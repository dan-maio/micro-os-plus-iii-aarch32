#!/usr/bin/env bash
# qemu3.sh — run a test on QEMU raspi3b (real Cortex-A53) via the AArch64 shim.
#
# Unlike qemu.sh (raspi2b / Cortex-A7), this runs the *default* -mcpu=cortex-a53
# build on QEMU's 64-bit BCM2837 model, so the true A53 (ARMv8 lda/stl atomics,
# SMPEN, cacheable+shareable coherency) is exercised in emulation.
#
# QEMU raspi3b boots AArch64; the shim (loaded as -kernel at 0x80000) sets SMPEN
# + timer access, releases the 3 secondary cores, and drops each to AArch32 SVC
# at 0x10000, where the AArch32 kernel (loaded via -device loader) takes over.
#
# SEMIHOST is enabled so the console is mirrored to QEMU (serial console is
# disabled to avoid duplicated output) and each test's semihosted SYS_EXIT
# ends the run with a RESULT (no infinite idle).
#
# The test is built here with the DEFAULT (A53) target + SEMIHOST=1.
# Usage: ./qemu3.sh <test-dir>            e.g. ./qemu3.sh smp_test2
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${1:-smp_test2}"
SHIM="$HERE/qemu-raspi3-shim/shim8.img"
[[ -f "$SHIM" ]] || make -C "$HERE/qemu-raspi3-shim" >/dev/null
( cd "$HERE/$DIR" && make SEMIHOST=1 )
IMG="$HERE/$DIR/output/kernel7.img"
QEMU64="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-aarch64 2>/dev/null | tail -1)"
[[ -x "$QEMU64" ]] || QEMU64=qemu-system-aarch64
exec "$QEMU64" -M raspi3b -smp 4 -nographic -serial none \
     -semihosting-config enable=on,target=native \
     -kernel "$SHIM" -device loader,file="$IMG",addr=0x10000
