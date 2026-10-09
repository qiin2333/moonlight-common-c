#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${TMPDIR:-/tmp}/moonlight-common-rtsp-dynamic-hdr-test
mkdir -p "$BUILD_DIR"
OPUS_INCLUDE=${OPUS_INCLUDE:-"$ROOT/../../libs/opus/include/opus"}
CC=${CC:-cc}
case "$(uname -s)" in
    Darwin) DEAD_STRIP=-Wl,-dead_strip ;;
    *) DEAD_STRIP=-Wl,--gc-sections ;;
esac

"$CC" -std=c11 -Wall -Wextra -Wno-unused-function \
    -ffunction-sections -fdata-sections \
    -I"$ROOT/src" -I"$ROOT/enet/include" -I"$OPUS_INCLUDE" \
    "$ROOT/src/DynamicHdr.c" "$ROOT/src/RtspParser.c" \
    "$ROOT/tests/rtsp_dynamic_hdr_test.c" "$DEAD_STRIP" \
    -o "$BUILD_DIR/rtsp_dynamic_hdr_test"

"$BUILD_DIR/rtsp_dynamic_hdr_test"
