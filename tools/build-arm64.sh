#!/bin/sh
set -eu
: "${CAPSULE_SYSROOT:?Set CAPSULE_SYSROOT to an Alpine aarch64 sysroot with musl-dev, linux-headers and the firmware musl library}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$HERE"
mkdir -p bin
clang --target=aarch64-linux-musl --sysroot="$CAPSULE_SYSROOT" \
    -Iinclude -Igenerated -Os -std=c11 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -fPIE -nostdlib -pie -fuse-ld=lld \
    -Wl,--gc-sections -Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1 \
    "$CAPSULE_SYSROOT/usr/lib/Scrt1.o" "$CAPSULE_SYSROOT/usr/lib/crti.o" \
    src/*.c -L"$CAPSULE_SYSROOT/lib" -L"$CAPSULE_SYSROOT/usr/lib" \
    -lc "$CAPSULE_SYSROOT/usr/lib/crtn.o" \
    -o bin/capsule-tweaker-aarch64
llvm-strip --strip-all bin/capsule-tweaker-aarch64
sha256sum bin/capsule-tweaker-aarch64 > bin/SHA256SUMS
