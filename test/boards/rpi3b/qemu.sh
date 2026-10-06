#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — same silicon as the Pi Zero 2 W, so the same script.
#
# This board differs from rpi-zero-2w by one linker script; it shares src/ and
# include/ with it, and has its own tests, and shares this runner too. Only the default
# build directory differs, because a separate BOARD gets a separate build.
# -----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
export BUILD="${BUILD:-$ROOT/test/build-rpi3b}"
# This board's tests are its own, even though the runner is shared.
export UOS_TEST_SRC_DIR="${UOS_TEST_SRC_DIR:-$ROOT/test/rpi3b}"
exec "$HERE/../rpi-zero-2w/qemu.sh" "$@"
