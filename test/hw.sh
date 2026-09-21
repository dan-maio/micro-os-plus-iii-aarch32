#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh — run this port's tests on BOARD.
#
# A dispatcher and nothing else. Every board carries its own test/boards/<id>/hw.sh
# with the same interface, so this file has no case statement and no list of
# board names: adding a board adds a directory, exactly as it does for
# board.cmake, src/, include/ and test/.
#
#     BOARD=<id> test/hw.sh [args...]   ->  test/boards/<id>/hw.sh [args...]
#
# Default BOARD is rpi-zero-2w.
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-rpi-zero-2w}"

SCRIPT="$HERE/boards/$BOARD/hw.sh"
if [[ ! -x "$SCRIPT" ]]; then
  echo "hw.sh: test/boards/$BOARD has no hw.sh" >&2
  echo "available boards:" >&2
  for d in "$HERE"/boards/*/; do
    b="$(basename "$d")"
    [[ -x "$d/hw.sh" ]] && echo "  $b" >&2
  done
  exit 2
fi

exec "$SCRIPT" "$@"
