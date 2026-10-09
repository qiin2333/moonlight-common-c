#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${TMPDIR:-/tmp}/moonlight-common-dynamic-hdr-test
mkdir -p "$BUILD_DIR"

# Limelight.h includes Opus's public header. Use a checkout-local Opus include
# directory when available, while allowing callers to override it.
OPUS_INCLUDE=${OPUS_INCLUDE:-"$ROOT/../../libs/opus/include/opus"}
CC=${CC:-cc}

"$CC" -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/src" -I"$ROOT/enet/include" -I"$OPUS_INCLUDE" \
    "$ROOT/src/DynamicHdr.c" "$ROOT/src/SdpGenerator.c" \
    "$ROOT/tests/dynamic_hdr_test.c" -o "$BUILD_DIR/dynamic_hdr_test"

"$BUILD_DIR/dynamic_hdr_test"
