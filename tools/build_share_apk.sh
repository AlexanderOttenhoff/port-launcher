#!/usr/bin/env bash
# Builds an APK that's safe to hand to other people: decomp modules only (no recomps: GPL code needing source
# alongside, and Banjo-Tooie's committed generated code is for local builds only), with each module's upstream
# licence files in its assets. Run the modules' build.sh first; this only re-stages what they staged.
#   tools/build_share_apk.sh   ->  bin/port-launcher-decomp-only.apk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FULL="$ROOT/build/modules/apk"
OUT="$ROOT/build/modules/apk-decomp"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"

rm -rf "$OUT"
mkdir -p "$OUT/jniLibs/arm64-v8a" "$OUT/assets/modules"
for manifest in "$ROOT"/modules/*/manifest.json; do
  dir="$(dirname "$manifest")"
  id="$(basename "$dir")"
  family="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("family",""))' "$manifest")"
  [ "$family" = "n64recomp" ] && continue
  [ -f "$FULL/jniLibs/arm64-v8a/libplm_$id.so" ] || { echo "skip $id (not built)"; continue; }
  cp "$FULL/jniLibs/arm64-v8a/libplm_$id.so" "$OUT/jniLibs/arm64-v8a/"
  cp -r "$FULL/assets/modules/$id" "$OUT/assets/modules/"
  # Upstream licence files: the port's own and, for libultraship ports, libultraship's.
  src="$(sed -n 's/^SRC="\$ROOT\/\(.*\)"$/\1/p' "$dir/prepare.sh")"
  for f in "$ROOT/$src"/{LICENSE,LICENSE.md,COPYING,NOTICE} "$ROOT/$src"/libultraship/LICENSE; do
    [ -f "$f" ] || continue
    rel="${f#"$ROOT/$src"/}"
    cp "$f" "$OUT/assets/modules/$id/$(echo "$rel" | tr '/' '-' | sed 's/^/upstream-/')"
  done
  echo "included $id ($family): $(ls "$OUT/assets/modules/$id" | grep '^upstream-' | tr '\n' ' ')"
done

[ -f "$ROOT/android/local.properties" ] || echo "sdk.dir=$ANDROID_HOME" > "$ROOT/android/local.properties"
"$ROOT/android/gradlew" -q -p "$ROOT/android" assembleDebug -Pplm.stage=apk-decomp
mkdir -p "$ROOT/bin"
cp "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" "$ROOT/bin/port-launcher-decomp-only.apk"
echo "APK: $ROOT/bin/port-launcher-decomp-only.apk"
