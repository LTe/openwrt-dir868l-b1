#!/bin/sh
# Usage: tests/test-iwinfo-wl-scan.sh PATCHED_IWINFO_SOURCE OPENWRT_TARGET_STAGING_DIR
set -eu
src=$(realpath "${1:?patched iwinfo source directory required}")
staging=$(realpath "${2:?target staging directory required}")
tests=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
"${HOSTCC:-cc}" -O1 -g -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -D_GNU_SOURCE -DUSE_WL -DUSE_WEXT \
    -I"$src" -I"$src/include" -I"$staging/usr/include" \
    "$tests/iwinfo-wl-scan.c" "$src/iwinfo_utils.c" -o "$work/test-parser"
"$work/test-parser"
