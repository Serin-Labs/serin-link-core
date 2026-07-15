#!/usr/bin/env bash
# Vendor the sl2 core into the ESPHome component directory (flattened
# includes). Thin wrapper over sync_vendor.sh; kept for muscle memory.
set -euo pipefail
exec "$(dirname "$0")/sync_vendor.sh" "$(dirname "$0")/../esphome/components/serin_link"
