#!/usr/bin/env bash
# Copies SDL 2.32.10's Java glue into the app as app.portlauncher.sdl2 (run tools/deps/build_sdl2.sh first).
# The Java glue and the native SDL that modules link must be the same SDL version.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/build/deps/work/SDL2-2.32.10"
DST="$ROOT/android/app/src/main/java/app/portlauncher/sdl2"
rm -rf "$DST" && mkdir -p "$DST"
for f in "$SRC"/android-project/app/src/main/java/org/libsdl/app/*.java; do
  sed -e 's/^package org\.libsdl\.app;/package app.portlauncher.sdl2;/' \
      -e 's/org\.libsdl\.app\./app.portlauncher.sdl2./g' "$f" > "$DST/$(basename "$f")"
done
cp "$SRC/LICENSE.txt" "$DST/LICENSE.txt"

# Android 14+ (targetSdk 34+) requires an export flag on runtime receivers; SDL 2.32's hidapi manager has
# none and hid_init() throws, disabling USB/Bluetooth HID controllers. SDL3 uses RECEIVER_EXPORTED too.
sed -i -E 's/^( *)mContext\.registerReceiver\((m[A-Za-z]+Broadcast), filter\);/\1if (Build.VERSION.SDK_INT >= 33) {\n\1    mContext.registerReceiver(\2, filter, Context.RECEIVER_EXPORTED);\n\1} else {\n\1    mContext.registerReceiver(\2, filter);\n\1}/' \
  "$DST/HIDDeviceManager.java"
[ "$(grep -c 'RECEIVER_EXPORTED' "$DST/HIDDeviceManager.java")" = 2 ] || { echo "receiver patch didn't apply" >&2; exit 1; }
grep -rn 'org\.libsdl\.app' "$DST" && { echo "unrenamed references left" >&2; exit 1; }
echo "SDL Java glue: $DST"
