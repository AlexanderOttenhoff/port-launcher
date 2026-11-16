#!/usr/bin/env bash
# Runs the Kotlin app's JVM unit tests (android/app/src/test) on the host: pure logic only (atomic writes, session
# pruning, mod file ownership, ...), no device or emulator. The APK workflow runs it on every push.
# Why a script and not just the Gradle task: one name for local runs and CI, like tools/test_core.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
[ -f "$ROOT/android/local.properties" ] || echo "sdk.dir=$ANDROID_HOME" > "$ROOT/android/local.properties"
"$ROOT/android/gradlew" -p "$ROOT/android" testDebugUnitTest
