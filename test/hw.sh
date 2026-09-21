#!/usr/bin/env bash
# hw.sh — run the AArch32 hardware tests on real silicon.
#
# This is only the port half: the session itself is driven by the shared
# runner in the kernel repository (test/run-hw.sh), which every port uses.
# Everything AArch32-specific is here.
#
# Two things differ per board, and both are below:
#
#   Raspberry Pi Zero 2 W / Pi 3 B  (BOARD=zero2w, the default)
#     Four Cortex-A53 in 32-bit mode. All four are examinable from the start
#     -- QEMU's and the GPU armstub's cores are all parked in code -- so the
#     runner halts, loads and resumes every one of them, zeroing the
#     __smp_spin[4] launch slots in between so a secondary that reaches its
#     parking loop before core 0 clears .bss cannot jump to a stale entry.
#     Entry 0x1003c. Resume means "force CPSR to 0x600001da (SVC, IRQ/FIQ
#     masked) and resume at the entry" -- unlike AArch64 the mode has to be
#     set explicitly.
#
#   Luckfox Lyra B  (BOARD=luckfox-lyra)
#     Three Cortex-A7, but only core 0 is a debug target at load time: cores
#     1 and 2 sit in the BootROM until the kernel clears their CRU reset bits,
#     which is why the board's OpenOCD config declares them -defer-examine.
#     So the runner drives core 0 alone. There is no __smp_spin: the
#     secondaries are released through the SRAM mailbox, not a spin table.
#     Entry 0x00200000. The miniloader hands core 0 over in SVC with the MMU
#     and caches ON, so the mode needs no forcing but the MMU and caches do
#     need turning off before load_image -- writing the image through a dirty
#     cache leaves DRAM holding something else.
#
# On hardware nothing boots through the QEMU shim: on the Pi config.txt sets
# arm_64bit=0 and the GPU starts the cores in AArch32; a Cortex-A7 has no
# other state to be in.
#
# PURE OpenOCD: no GDB, no reset, and the UART console is NOT touched --
# keep your own terminal on it (/dev/ttyACM0 on the Pi, the Lyra's debug
# header at 1.5 Mbaud).
#
# ONE TEST PER POWER CYCLE: the run is a debug-in-RAM load over whatever the
# previous test left in DRAM, and neither board has a reset a script can
# drive. Run one test, power-cycle the board, run the next.
#
# Usage:  test/hw.sh <app> [run-seconds]
#         test/hw.sh list          the tests this build has
# Env:    BOARD=zero2w (default) | rpi3b | luckfox-lyra
#         PROBE=jlink (default) | olimex      -- the Pi boards only
#         BUILD=<dir>  the CMake build directory
#                      (default: build, or build-lyra for the Lyra)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BOARD="${BOARD:-zero2w}"
PROBE="${PROBE:-jlink}"

# gcc 15 xPack binutils, as the builds use; PATH otherwise.
TC="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.*/.content/bin 2>/dev/null | sort -V | tail -1)"

SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"

case "$BOARD" in
  luckfox-lyra)
    BUILD="${BUILD:-$ROOT/build-lyra}"
    CFG="$ROOT/boards/luckfox-lyra/openocd.cfg"

    # Core 0's MMU and caches, off, before anything is written to DRAM:
    # clear SCTLR.{M,C,I} (0x1005), invalidate the branch predictor and the
    # I-cache, and the TLB. This is the same sequence the board's own
    # write_board.sh has always used.
    SANITIZE='if {[catch {
      set _t [target current]
      set _s [$_t arm mrc 15 0 1 0 0]
      $_t arm mcr 15 0 1 0 0 [expr {$_s & ~0x1005}]
      $_t arm mcr 15 0 7 5 0 0
      $_t arm mcr 15 0 7 5 6 0
      $_t arm mcr 15 0 8 7 0 0
    } _e]} { echo "SANITIZE-ERR: $_e" } else { echo [format "SANITIZE-OK: SCTLR 0x%08x -> MMU/caches off" $_s] }'

    UOS_HW_CFG="$CFG" \
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
    UOS_HW_ADAPTER_KHZ="${UOS_HW_ADAPTER_KHZ:-1000}" \
    exec "$SMP_DIR/test/run-hw.sh" "$BUILD/test" "${1:-list}" "${2:-}"
    ;;

  zero2w|rpi3b)
    BUILD="${BUILD:-$ROOT/build}"
    case "$PROBE" in
      jlink)  CFG="$ROOT/boards/rpi-zero-2w/openocd-jlink-rpi3.cfg" ;;
      olimex) CFG="$ROOT/boards/rpi-zero-2w/openocd-olimex.cfg" ;;
      *) echo "hw.sh: PROBE must be jlink or olimex, not '$PROBE'" >&2; exit 2 ;;
    esac

    UOS_HW_CFG="$CFG" \
    UOS_HW_CFG_INIT=0 \
    UOS_HW_NM="${TC:+$TC/}arm-none-eabi-nm" \
    UOS_HW_READELF="${TC:+$TC/}arm-none-eabi-readelf" \
    UOS_HW_ENTRY=0x1003c \
    UOS_HW_SPIN_WORDS=4 \
    UOS_HW_RESUME=cpsr \
    UOS_HW_NCPU=4 \
    UOS_HW_TARGET_FMT="bcm2837.cpu%d" \
    exec "$SMP_DIR/test/run-hw.sh" "$BUILD/test" "${1:-list}" "${2:-}"
    ;;

  *)
    echo "hw.sh: BOARD must be zero2w, rpi3b or luckfox-lyra, not '$BOARD'" >&2
    exit 2
    ;;
esac
