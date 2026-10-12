#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/serin-link-host-tests.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
cd "$(dirname "$0")"
CFLAGS="-std=c11 -Wall -Wextra -Werror -I../include"
gcc $CFLAGS test_sl2_rxq.c -o "$BUILD_DIR/test_sl2_rxq"
"$BUILD_DIR/test_sl2_rxq"
RXQ_FLAGS=""
if [[ "${RUN_TSAN:-0}" == 1 ]]; then RXQ_FLAGS="-fsanitize=thread -g"; fi
g++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread $RXQ_FLAGS -I../include \
    test_sl2_rxq_thread.cpp -o "$BUILD_DIR/test_sl2_rxq_thread"
"$BUILD_DIR/test_sl2_rxq_thread"
gcc $CFLAGS test_sl2_proto.c -o "$BUILD_DIR/test_sl2_proto" -lm
"$BUILD_DIR/test_sl2_proto"
gcc $CFLAGS test_sl2_pair_auth.c -o "$BUILD_DIR/test_sl2_pair_auth" -lm
"$BUILD_DIR/test_sl2_pair_auth"
gcc $CFLAGS test_sl2_info.c -o "$BUILD_DIR/test_sl2_info" -lm
"$BUILD_DIR/test_sl2_info"
gcc $CFLAGS test_sl2_link.c ../src/sl2_link.c -o "$BUILD_DIR/test_sl2_link" -lm
"$BUILD_DIR/test_sl2_link"

# Vendored Monocypher (ESPHome component only -- the dial uses libsodium).
# Third-party sources compile under their own warning flags, not our -Werror.
MC=../esphome/components/serin_link
gcc -std=c11 -O2 -c $MC/monocypher.c -o "$BUILD_DIR/monocypher.o"
gcc -std=c11 -O2 -c $MC/monocypher-ed25519.c -I$MC -o "$BUILD_DIR/monocypher-ed25519.o"
gcc $CFLAGS -I$MC test_crypto_vectors.c "$BUILD_DIR/monocypher.o" "$BUILD_DIR/monocypher-ed25519.o" \
    -o "$BUILD_DIR/test_crypto_vectors" -lm
"$BUILD_DIR/test_crypto_vectors"

# Compile the actual ESPHome adapter with host platform/entity/storage stubs.
gcc $CFLAGS -c ../src/sl2_link.c -o "$BUILD_DIR/sl2_link.o"
g++ -std=c++17 -Wall -Werror -Iadapter/stubs -I$MC \
    test_adapter_source_health.cpp $MC/serin_link.cpp "$BUILD_DIR/sl2_link.o" \
    "$BUILD_DIR/monocypher.o" "$BUILD_DIR/monocypher-ed25519.o" \
    -o "$BUILD_DIR/test_adapter_source_health" -lm
"$BUILD_DIR/test_adapter_source_health"
g++ -std=c++17 -Wall -Werror -Iadapter/stubs -I$MC \
    test_adapter_source_reconcile.cpp $MC/serin_link.cpp "$BUILD_DIR/sl2_link.o" \
    "$BUILD_DIR/monocypher.o" "$BUILD_DIR/monocypher-ed25519.o" \
    -o "$BUILD_DIR/test_adapter_source_reconcile" -lm
"$BUILD_DIR/test_adapter_source_reconcile"
g++ -std=c++17 -Wall -Werror -Iadapter/stubs -I$MC \
    test_adapter_transport.cpp $MC/serin_link.cpp "$BUILD_DIR/sl2_link.o" \
    "$BUILD_DIR/monocypher.o" "$BUILD_DIR/monocypher-ed25519.o" \
    -o "$BUILD_DIR/test_adapter_transport" -lm
"$BUILD_DIR/test_adapter_transport"
g++ -std=c++17 -Wall -Werror -Iadapter/stubs -I$MC \
    test_adapter_fan.cpp $MC/serin_link.cpp "$BUILD_DIR/sl2_link.o" \
    "$BUILD_DIR/monocypher.o" "$BUILD_DIR/monocypher-ed25519.o" \
    -o "$BUILD_DIR/test_adapter_fan" -lm
"$BUILD_DIR/test_adapter_fan"
g++ -std=c++17 -Wall -Werror -Iadapter/stubs -I$MC \
    test_adapter_switch_restore.cpp $MC/serin_link.cpp "$BUILD_DIR/sl2_link.o" \
    "$BUILD_DIR/monocypher.o" "$BUILD_DIR/monocypher-ed25519.o" \
    -o "$BUILD_DIR/test_adapter_switch_restore" -lm
"$BUILD_DIR/test_adapter_switch_restore"
