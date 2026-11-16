#!/usr/bin/env bash
# Boots the test emulator the way the agent sessions use it, and waits until it's ready.
#   tools/emulator.sh [avd]        (default AVD: Medium_Phone)
#
# - Headless (-no-window): no window means no stray mouse clicks or scrolls reaching the app as touches while a
#   scripted test runs (owner 2026-11-13, after a scroll opened a confirmation dialog mid-test). Drive it with
#   `adb -s emulator-5554 shell input ...` and look with `adb exec-out screencap -p`.
# - Landscape, like the RP6 (owner 2026-11-13): auto-rotate off, rotation 1 (90°). Set after boot; the settings
#   persist in the AVD's userdata.
# - Never run it during a heavy native build (32 GB, no swap: see the build memory cap in modules/*/build.sh).
set -euo pipefail
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
AVD="${1:-Medium_Phone}"
ADB="$ANDROID_HOME/platform-tools/adb"
SERIAL=emulator-5554

if ! "$ADB" devices | grep -q "^$SERIAL"; then
  "$ANDROID_HOME/emulator/emulator" -avd "$AVD" -no-window -no-snapshot-save -no-audio -no-boot-anim >/dev/null 2>&1 &
fi
for _ in $(seq 100); do
  [ "$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
  sleep 3
done
"$ADB" -s "$SERIAL" shell settings put system accelerometer_rotation 0
"$ADB" -s "$SERIAL" shell settings put system user_rotation 1
echo "$SERIAL ready (headless, landscape)"
