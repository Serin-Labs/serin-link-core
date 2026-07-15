#!/usr/bin/env bash
# Vendor the sl2 core flat into a target directory.
#
# Adopters compile the vendored files directly (ESPHome components and ESP-IDF
# main/ dirs provide no include-path hook for the canonical include/serin_link/
# tree), so the headers are copied flat and the one nested include in
# sl2_link.c is rewritten. Re-run after editing the core; vendored copies are
# committed in the adopter repo. sl2_proto.h / sl2_sha256.h must stay
# byte-identical across every tree (the dial firmware vendors those two).
#
# Usage: tools/sync_vendor.sh <dst-dir>
#   e.g. tools/sync_vendor.sh ../mitsubishi-cn105-homekit/main
set -euo pipefail
DST="$(realpath "${1:?usage: tools/sync_vendor.sh <dst-dir>}")"
[ -d "$DST" ] || { echo "ERROR: $DST is not a directory" >&2; exit 1; }
cd "$(dirname "$0")/.."
for h in sl2_proto.h sl2_crypto.h sl2_sha256.h sl2_port.h sl2_bond.h sl2_rxq.h sl2_link.h; do
    cp "include/serin_link/$h" "$DST/"
done
sed 's|#include "serin_link/sl2_link.h"|#include "sl2_link.h"|' \
    src/sl2_link.c > "$DST/sl2_link.c"
echo "synced sl2 core -> $DST"
