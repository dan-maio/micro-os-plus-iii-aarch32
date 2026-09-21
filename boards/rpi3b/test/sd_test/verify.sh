#!/usr/bin/env bash
# verify.sh — host-side check of the files the firmware wrote to disk.img
# ("device -> host" direction). Run after ./run.sh has formatted the card.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$HERE/flatfs_tool.py" verify "$HERE/disk.img"
