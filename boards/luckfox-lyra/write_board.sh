#!/usr/bin/env bash
# write_board.sh — ATTACH-ONLY loader for RK3506 Cortex-A7 (Luckfox Lyra)
# Halts core 0, disables caches/MMU, loads binary to DDR, verifies, and resumes.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/output/luckfox_lyra_micro_os.bin"
ENTRY=0x00200000
RETRIES=3
ADAPTER_KHZ=1000
COLD=0

# Command line args
for arg in "$@"; do
  if [ "$arg" = "--cold" ]; then
    COLD=1
  fi
done

# Locate OpenOCD (auto-detects xPack or system PATH)
OPENOCD=$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/openocd/*/.content/bin/openocd 2>/dev/null | sort -V | tail -1 || echo "openocd")

if [ ! -f "$BIN" ]; then
  echo "Error: missing binary '$BIN'. Run 'make' first."
  exit 1
fi

echo "================================================================================"
echo "== ATTACH-ONLY FLASHING PROCESS"
echo "== Target: Core 0 (Cores 1/2 left parked in BootROM)"
echo "== Image:  $BIN"
echo "== Address: $ENTRY"
echo "================================================================================"

# Power cycle warning: cores 1/2 mailbox release is a one-shot operation
if [ "$COLD" -ne 1 ]; then
  echo "==> Please POWER-CYCLE the board now (resets mailbox state), then press ENTER"
  read -r
  sleep 2 # Let miniloader initialize DDR and UART1
fi

# Tcl commands to sanitize core 0's caches & MMU before writing to DDR
SANITIZE='if {[catch { set _t [target current]; set _s [$_t arm mrc 15 0 1 0 0]; $_t arm mcr 15 0 1 0 0 [expr {$_s & ~0x1005}]; $_t arm mcr 15 0 7 5 0 0; $_t arm mcr 15 0 7 5 6 0; $_t arm mcr 15 0 8 7 0 0 } _e]} { echo "SANITIZE-ERR: $_e" } else { echo [format "SANITIZE-OK: SCTLR 0x%08x (core0 only) -> MMU/caches off" $_s] }'

echo "== OpenOCD: Halting core 0 -> sanitize -> loading bin -> verify -> resume @$ENTRY"
ok=0

for try in $(seq 1 "$RETRIES"); do
  echo "--> Attempt $try/$RETRIES..."
  pkill -x openocd 2>/dev/null || true
  sleep 1

  # Run OpenOCD loading pipeline
  if "$OPENOCD" -f "$HERE/openocd.cfg" \
      -c "halt" -c "adapter speed $ADAPTER_KHZ" \
      -c "$SANITIZE" \
      -c "load_image $BIN $ENTRY bin" \
      -c "verify_image $BIN $ENTRY bin" \
      -c "resume $ENTRY" \
      -c "sleep 200" \
      -c "shutdown" 2>&1; then
    ok=1
    echo "== SUCCESS: loaded, verified, and resumed OK =="
    break
  fi
  echo "--> Attempt $try failed."
done

if [ "$ok" -ne 1 ]; then
  echo "== ERROR: Flashing failed after $RETRIES attempts =="
  exit 1
fi
