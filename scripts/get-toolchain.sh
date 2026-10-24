#!/bin/sh
# Download OpenWrt's prebuilt external toolchain for the target (what upstream CI does instead of
# building gcc): prints the directory to pass as TOOLCHAIN_DIR.  Usage: scripts/get-toolchain.sh [dest]
set -e
TARGET=${TARGET:-bcm53xx/generic}
BASE=${TOOLCHAIN_BASE:-https://downloads.cdn.openwrt.org/snapshots/targets/$TARGET}
DEST=${1:-toolchain}
mkdir -p "$DEST"
file=$(curl -fsSL "$BASE/sha256sums" | sed -n -E 's/.*\*(openwrt-toolchain-.*\.tar\.(xz|zst))$/\1/p' | head -n 1)
[ -n "$file" ] || { echo "no toolchain tarball listed in $BASE/sha256sums" >&2; exit 1; }
[ -f "$DEST/$file" ] || curl -fsSL -o "$DEST/$file" "$BASE/$file"
case "$file" in
	*.zst) tar -C "$DEST" --zstd -xf "$DEST/$file" ;;
	*)     tar -C "$DEST" -xf "$DEST/$file" ;;
esac
echo "$(cd "$DEST" && pwd)/${file%.tar.*}"
