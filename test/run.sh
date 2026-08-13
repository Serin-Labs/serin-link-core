#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_sl2_proto /tmp/test_sl2_info /tmp/test_sl2_link \
            /tmp/test_crypto_vectors /tmp/monocypher.o /tmp/monocypher-ed25519.o' EXIT
cd "$(dirname "$0")"
CFLAGS="-std=c11 -Wall -Wextra -Werror -I../include"
gcc $CFLAGS test_sl2_proto.c -o /tmp/test_sl2_proto -lm
/tmp/test_sl2_proto
gcc $CFLAGS test_sl2_info.c -o /tmp/test_sl2_info -lm
/tmp/test_sl2_info
gcc $CFLAGS test_sl2_link.c ../src/sl2_link.c -o /tmp/test_sl2_link -lm
/tmp/test_sl2_link

# Vendored Monocypher (ESPHome component only -- the dial uses libsodium).
# Third-party sources compile under their own warning flags, not our -Werror.
MC=../esphome/components/serin_link
gcc -std=c11 -O2 -c $MC/monocypher.c -o /tmp/monocypher.o
gcc -std=c11 -O2 -c $MC/monocypher-ed25519.c -I$MC -o /tmp/monocypher-ed25519.o
gcc $CFLAGS -I$MC test_crypto_vectors.c /tmp/monocypher.o /tmp/monocypher-ed25519.o \
    -o /tmp/test_crypto_vectors -lm
/tmp/test_crypto_vectors
