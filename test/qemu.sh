#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — run this port's tests on BOARD.
#
# A dispatcher and nothing else. Every board carries its own test/boards/<id>/qemu.sh
# with the same interface, so this file has no case statement and no list of
# board names: adding a board adds a directory, exactly as it does for
# board.cmake, src/, include/ and test/.
#
#     BOARD=<id> test/qemu.sh [args...]   ->  test/boards/<id>/qemu.sh [args...]
#
# Default BOARD is rpi-zero-2w.
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-rpi-zero-2w}"

SCRIPT="$HERE/boards/$BOARD/qemu.sh"
if [[ ! -x "$SCRIPT" ]]; then
  # Two different misses, and they deserve different answers. A board that
  # exists but carries no qemu.sh is a hardware-only board -- QEMU models
  # nothing useful of its silicon, so it has no emulated suite and its
  # board.cmake sets no QEMU linker script either. Anything else is a name
  # that is not a board of this port.
  if [[ -f "$HERE/boards/$BOARD/board.cmake" ]]; then
    echo "qemu.sh: $BOARD is tested on hardware -- it has no emulated suite." >&2
    echo "         BOARD=$BOARD test/hw.sh <test>" >&2
  else
    echo "qemu.sh: $BOARD is not a board of this port" >&2
  fi
  echo "boards with an emulated suite:" >&2
  for d in "$HERE"/boards/*/; do
    b="$(basename "$d")"
    [[ -x "$d/qemu.sh" ]] && echo "  $b" >&2
  done
  exit 2
fi

exec "$SCRIPT" "$@"
