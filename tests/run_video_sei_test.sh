#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${TMPDIR:-/tmp}/moonlight-common-video-sei-test
mkdir -p "$BUILD_DIR"
OPUS_INCLUDE=${OPUS_INCLUDE:-"$ROOT/../../libs/opus/include/opus"}
CC=${CC:-cc}
case "$(uname -s)" in
    Darwin) DEAD_STRIP=-Wl,-dead_strip ;;
    *) DEAD_STRIP=-Wl,--gc-sections ;;
esac

"$CC" -std=c11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -ffunction-sections -fdata-sections \
    -I"$ROOT/src" -I"$ROOT/enet/include" -I"$OPUS_INCLUDE" \
    "$ROOT/tests/video_sei_test.c" "$DEAD_STRIP" \
    -o "$BUILD_DIR/video_sei_test"

"$BUILD_DIR/video_sei_test"
