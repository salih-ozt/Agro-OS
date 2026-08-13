#!/usr/bin/env bash
set -euo pipefail

AGROOS="$(cd "$(dirname "$0")/.." && pwd)"
SKIA="$AGROOS/skia_prebuilt"
COMMON="$AGROOS/ui/common"
SRC="$AGROOS/ui/settings.cpp"
OUT="$AGROOS/ui/settings/settings_bin"
TMP="$AGROOS/ui/settings/build_tmp"

mkdir -p "$TMP"

gcc -c "$COMMON/xdg-shell-protocol.c" -I"$COMMON" -o "$TMP/xdg-shell-protocol.o"

g++ -D_GNU_SOURCE -std=c++17 \
  -I"$SKIA" \
  -I"$COMMON" \
  -I"$AGROOS/ui" \
  -c "$SRC" \
  -o "$TMP/settings.o"

g++ "$TMP/settings.o" "$TMP/xdg-shell-protocol.o" \
  -o "$OUT" \
  "$SKIA/out/Release/libskia.a" \
  "$SKIA/out/Release/libskcms.a" \
  "$SKIA/out/Release/libfreetype2.a" \
  "$SKIA/out/Release/libpng.a" \
  "$SKIA/out/Release/libjpeg.a" \
  "$SKIA/out/Release/libzlib.a" \
  "$SKIA/out/Release/libexpat.a" \
  -lwayland-client -lfontconfig -lGL -lEGL -ldl -lpthread -lfreetype

echo "BUILT: $OUT"
