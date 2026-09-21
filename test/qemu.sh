#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — run this port's tests on BOARD.
#
# A dispatcher and nothing else. Every board carries its own boards/<id>/qemu.sh
# with the same interface, so this file has no case statement and no list of
# board names: adding a board adds a directory, exactly as it does for
# board.cmake, src/, include/ and test/.
#
#     BOARD=<id> test/qemu.sh [args...]   ->  boards/<id>/qemu.sh [args...]
#
# Default BOARD is rpi-zero-2w.
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BOARD="${BOARD:-rpi-zero-2w}"

SCRIPT="$ROOT/boards/$BOARD/qemu.sh"
if [[ ! -x "$SCRIPT" ]]; then
  echo "qemu.sh: boards/$BOARD has no qemu.sh" >&2
  echo "available boards:" >&2
  for d in "$ROOT"/boards/*/; do
    b="$(basename "$d")"
    [[ -x "$d/qemu.sh" ]] && echo "  $b" >&2
  done
  exit 2
fi

exec "$SCRIPT" "$@"
