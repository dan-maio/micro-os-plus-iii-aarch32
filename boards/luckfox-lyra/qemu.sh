#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — run this board's tests under QEMU.
#
#     boards/<id>/qemu.sh            the whole suite
#     boards/<id>/qemu.sh <app>      one test
#
# The machine is `virt`, not a model of this board: QEMU emulates none of the
# RK3506's own blocks -- the CRU, the SRAM mailbox, the Rockchip gadget, the
# Cortex-M0 -- so emulating the Lyra would mean emulating the parts least like
# a Cortex-A7. `virt` is a generic A7 with a PL011 at 0x09000000, a GICv2 at
# 0x08000000 and DRAM at 0x40000000, which is what the -qemu images are linked
# for. The emulated runs therefore cover the SoC-neutral subset only.
#
# No boot shim: a Cortex-A7 has no AArch64 state to drop out of.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: ../../build-lyra)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-lyra}"
SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"

QEMU="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-arm 2>/dev/null | sort -V | tail -1)"
[[ -x "$QEMU" ]] || QEMU=qemu-system-arm

UOS_TEST_SRC_DIR="$HERE/test" \
UOS_QEMU_ONLY="${1:-}" \
exec "$SMP_DIR/test_smpl/run-qemu.sh" "$BUILD/test" "$QEMU" \
     -M virt -cpu cortex-a7 -smp 3 -m 128M
