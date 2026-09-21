#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh — same silicon as the Pi Zero 2 W, so the same script.
#
# This board differs from rpi-zero-2w by one linker script; it shares src/,
# include/ and test/ with it, and shares this runner too. Only the default
# build directory differs, because a separate BOARD gets a separate build.
# -----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export BUILD="${BUILD:-$(cd "$HERE/../.." && pwd)/build-rpi3b}"
exec "$HERE/../rpi-zero-2w/hw.sh" "$@"
