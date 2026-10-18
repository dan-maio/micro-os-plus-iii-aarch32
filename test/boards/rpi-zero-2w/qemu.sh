#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — run this board's tests under QEMU.
#
# Every board has this script, with this interface:
#
#     test/boards/<id>/qemu.sh            the whole suite
#     test/boards/<id>/qemu.sh <app>      one test
#
# The machine is the board fact. This one is `raspi3b` plus a shim: QEMU starts
# its Cortex-A53 cores in AArch64 and this is the 32-bit port, so the image is
# loaded at 0x10000 and a 20-line stub drops to AArch32 and jumps there.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: test/build)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build}"
# The kernel is the sibling micro-os-plus-iii (the smp branch of
# micro-os-plus-iii-smp.git); an older workspace may keep it as
# micro-os-plus-iii-smp, or as a `*.git` working copy of either name.
_siblings="$(cd "$ROOT/.." && pwd)"
SMP_DIR="${UOS_SMP_DIR:-}"
if [[ -z "$SMP_DIR" ]]; then
  for _d in micro-os-plus-iii micro-os-plus-iii.git \
            micro-os-plus-iii-smp micro-os-plus-iii-smp.git; do
    if [[ -d "$_siblings/$_d" ]]; then SMP_DIR="$_siblings/$_d"; break; fi
  done
  SMP_DIR="${SMP_DIR:-$_siblings/micro-os-plus-iii}"
fi

QEMU="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-aarch64 2>/dev/null | sort -V | tail -1)"
[[ -x "$QEMU" ]] || QEMU=qemu-system-aarch64

SHIM="$BUILD/test/shim8.img"
[[ -f "$SHIM" ]] || { echo "qemu.sh: no boot shim at $SHIM -- build first" >&2; exit 2; }

UOS_QEMU_SHIM="$SHIM" \
UOS_QEMU_LOAD_ADDR=0x10000 \
UOS_TEST_SRC_DIR="${UOS_TEST_SRC_DIR:-$ROOT/test/rpi-zero-2w}" \
UOS_QEMU_ONLY="${1:-}" \
exec "$SMP_DIR/test_smpl/run-qemu.sh" "$BUILD/test" "$QEMU" -M raspi3b -smp 4
