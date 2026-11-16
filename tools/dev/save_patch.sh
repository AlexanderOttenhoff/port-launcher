#!/usr/bin/env bash
# Commits the upstream checkout's working changes as user "plm" and saves them as the module's next numbered patch
# (a fresh prepare.sh reapplies modules/<id>/patches/<repo>/*.patch in order).
#   tools/dev/save_patch.sh goldeneye goldeneye-pc-port "Android: what and why"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
id="$1"; repo="$2"; msg="$3"
dir="$ROOT/modules/$id/patches/$repo"
mkdir -p "$dir"
last=$(ls "$dir" 2>/dev/null | sed -n 's/^\([0-9]\{4\}\)-.*/\1/p' | sort -n | tail -1)
next=$(( 10#${last:-0} + 1 ))
git -C "$ROOT/build/upstream/$repo" add -A
git -C "$ROOT/build/upstream/$repo" -c user.name=plm -c user.email=plm@localhost commit -q -m "$msg"
git -C "$ROOT/build/upstream/$repo" format-patch -1 -o "$dir" --start-number "$next" | sed "s|$ROOT/||"
