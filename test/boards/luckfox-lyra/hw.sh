#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh — run ONE test on this board, over OpenOCD.
#
# Every board has this script, with this interface:
#
#     test/boards/<id>/hw.sh list                 the tests this build has
#     test/boards/<id>/hw.sh <app> [run-seconds]  run one
#
# The session is the shared runner (test_smpl/run-hw.sh); everything here is an
# RK3506 fact.
#
# PURE OpenOCD. No GDB, no reset, and the UART is NOT touched -- keep your own
# terminal on the Lyra's debug UART at 115200.
#
# ONE TEST PER POWER CYCLE: the secondaries are released once, by clearing
# __smp_spin, and a second load cannot un-release them.
#
# Env:  BUILD=<dir>            the CMake build directory  (default: test/build-lyra)
#       UOS_HW_ADAPTER_KHZ=<n> SWD clock                  (default: 4000)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build-lyra}"

TC="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.*/.content/bin 2>/dev/null | sort -V | tail -1)"
SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"

# The miniloader leaves the MMU and caches on, with its own page tables. The
# image is loaded over that, so the core is put back the way a reset leaves it
# before anything is written: SCTLR.M/C/I cleared, then TLBs and caches
# invalidated. Without this the load lands in a stale mapping.
SANITIZE='if {[catch {
  set _t [target current]
  set _s [$_t arm mrc 15 0 1 0 0]
  $_t arm mcr 15 0 1 0 0 [expr {$_s & ~0x1005}]
  $_t arm mcr 15 0 7 5 0 0
  $_t arm mcr 15 0 7 5 6 0
  $_t arm mcr 15 0 8 7 0 0
} _e]} { echo "SANITIZE-ERR: $_e" } else { echo [format "SANITIZE-OK: SCTLR 0x%08x -> MMU/caches off" $_s] }'

# The RK3506 facts, and only those:
#   * entry 0x00200000, where the Lyra image is linked;
#   * only core 0 is a debug target at load time -- the other two A7s are
#     released by the image itself, so there are no spin words to zero;
#   * resume means "resume at the entry", the miniloader having left the
#     cores in a sane state;
#   * this openocd.cfg runs `init` itself.
UOS_HW_CFG="$HERE/openocd.cfg" \
UOS_HW_CFG_INIT=0 \
UOS_HW_NM="${TC:+$TC/}arm-none-eabi-nm" \
UOS_HW_READELF="${TC:+$TC/}arm-none-eabi-readelf" \
UOS_HW_ENTRY=0x00200000 \
UOS_HW_SPIN_WORDS=0 \
UOS_HW_RESUME=entry \
UOS_HW_NCPU=3 \
UOS_HW_CORES="0" \
UOS_HW_TARGET_FMT="rk3506.a7.%d" \
UOS_HW_PRELOAD="$SANITIZE" \
UOS_HW_ADAPTER_KHZ="${UOS_HW_ADAPTER_KHZ:-4000}" \
exec "$SMP_DIR/test_smpl/run-hw.sh" "$BUILD/test" "${1:-list}" "${2:-}"
