#!/usr/bin/env bash
# Builds the launcher APK (debug by default) and copies it to bin/.
#   tools/build_apk.sh [debug|release] [gradle args...]
# No modules are bundled (they come from the module server); pass -Pplm.stage=apk to bundle what modules/*/build.sh
# staged in build/modules/apk, for a dev build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:-debug}"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"

[ -f "$ROOT/android/local.properties" ] || echo "sdk.dir=$ANDROID_HOME" > "$ROOT/android/local.properties"

# Port icons for the library rows (from the upstream checkouts, never committed).
python3 "$ROOT/tools/stage_icons.py"

TASK="assemble${VARIANT^}"
"$ROOT/android/gradlew" -p "$ROOT/android" "$TASK" "${@:2}"

mkdir -p "$ROOT/bin"
APK="$(find "$ROOT/android/app/build/outputs/apk/$VARIANT" -name '*.apk' | head -n1)"
cp "$APK" "$ROOT/bin/port-launcher-$VARIANT.apk"
echo "APK: $ROOT/bin/port-launcher-$VARIANT.apk"
