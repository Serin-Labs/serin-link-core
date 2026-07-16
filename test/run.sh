#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_sl2_proto /tmp/test_sl2_info /tmp/test_sl2_link' EXIT
cd "$(dirname "$0")"
CFLAGS="-std=c11 -Wall -Wextra -Werror -I../include"
gcc $CFLAGS test_sl2_proto.c -o /tmp/test_sl2_proto -lm
/tmp/test_sl2_proto
gcc $CFLAGS test_sl2_info.c -o /tmp/test_sl2_info -lm
/tmp/test_sl2_info
gcc $CFLAGS test_sl2_link.c ../src/sl2_link.c -o /tmp/test_sl2_link -lm
/tmp/test_sl2_link
