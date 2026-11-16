#!/usr/bin/env bash
# Builds the debug APK with the memory cap, prints only errors, checks the APK is new, and with --install puts it on
# the connected device (ask the owner first: never install while they're playing).   tools/dev/apk.sh [--install]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/logs
start=$(date +%s)
if systemd-run --user --scope -q -p MemoryMax=10G -p MemorySwapMax=0 tools/build_apk.sh >build/logs/apk.log 2>&1; then
  echo "OK      apk"
else
  echo "FAILED  apk   (full log: build/logs/apk.log)"; grep -E "^e: |error:" build/logs/apk.log | head -20; exit 1
fi
grep -E "^w: .*(GameMenu|ModsPage|LibraryController)" build/logs/apk.log | head -5
apk=bin/port-launcher-debug.apk
[ "$(stat -c %Y "$apk")" -ge "$start" ] || { echo "APK is older than this build: $apk"; exit 1; }
if [ "${1:-}" = "--install" ]; then
  "${ANDROID_HOME:-$HOME/Android/Sdk}/platform-tools/adb" install -r "$apk" 2>&1 | tail -1
fi
