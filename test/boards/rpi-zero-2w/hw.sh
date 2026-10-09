#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh — run ONE test on this board, over OpenOCD.
#
# Every board has this script, with this interface:
#
#     test/boards/<id>/hw.sh list                 the tests this build has
#     test/boards/<id>/hw.sh <app> [run-seconds]  run one
#
# All of it is board facts. The session itself is the shared runner,
# micro-os-plus-iii-smp/test_smpl/run-hw.sh, which is the same file for every
# board of every port: halt, enable semihosting, load_image, resume. Nothing
# board-specific lives there and nothing generic lives here.
#
# PURE OpenOCD. No GDB, no reset, and the UART is NOT touched -- keep your own
# terminal on it (tio -b 115200 /dev/ttyACM0).
#
# ONE TEST PER POWER CYCLE. The run is a load into RAM over whatever the
# previous test left there, and this board has no SRST a script can drive.
#
# Env:  BUILD=<dir>   the CMake build directory        (default: ../../build)
#       PROBE=jlink | olimex                           (default: jlink)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build}"
PROBE="${PROBE:-jlink}"

case "$PROBE" in
  jlink)  CFG="$HERE/openocd-jlink-rpi3.cfg" ;;
  olimex) CFG="$HERE/openocd-olimex.cfg" ;;
  *) echo "hw.sh: PROBE must be jlink or olimex, not '$PROBE'" >&2; exit 2 ;;
esac

TC="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.*/.content/bin 2>/dev/null | sort -V | tail -1)"
SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"
# A workspace may keep the sibling as a `*.git` working copy.
[ -d "$SMP_DIR" ] || SMP_DIR="$SMP_DIR.git"

# The BCM2837 facts, and only those:
#   * entry 0x1003c, where the AArch32 image is linked;
#   * only core 0 is started ("set CPSR then PC"). Cores 1-3 are resumed
#     where they were halted: in the firmware's 32-bit arm stub after the
#     watchdog reset, waiting on their mailbox 3, until the kernel releases
#     them (smp.cpp) once it has written their __smp_spin slot. So
#     __smp_spin is not zeroed here. This needs an SD card that boots
#     32-bit (arm_64bit=0) with a kernel that leaves cores 1-3 in that
#     loop; the previous all-cores flow is kept in hw.sh.bak;
#   * the OpenOCD configs here run `init` themselves.
UOS_HW_CFG="$CFG" \
UOS_HW_CFG_INIT=0 \
UOS_HW_NM="${TC:+$TC/}arm-none-eabi-nm" \
UOS_HW_READELF="${TC:+$TC/}arm-none-eabi-readelf" \
UOS_HW_ENTRY=0x1003c \
UOS_HW_SPIN_WORDS=0 \
UOS_HW_RESUME=cpsr-first \
UOS_HW_NCPU=4 \
UOS_HW_TARGET_FMT="bcm2837.cpu%d" \
UOS_HW_PRELOAD="${UOS_HW_PRELOAD:-mww 0x3f100024 0x5a000001; mww 0x3f10001c 0x5a000020}" \
exec "$SMP_DIR/test_smpl/run-hw.sh" "$BUILD/test" "${1:-list}" "${2:-}"
