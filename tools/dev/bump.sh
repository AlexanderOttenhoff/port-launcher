#!/usr/bin/env bash
# Bumps a module's "+plm.N" version (the app offers the update when it changes).   tools/dev/bump.sh goldeneye [...]
# The build stages its own copy of the manifest (build/modules/apk/assets/modules/<id>/manifest.json) and pack.sh
# reads that copy, so the staged version is updated too: bumping after a build no longer packs the old version.
#   tools/dev/bump.sh --sync goldeneye [...]   only copies the current version into the staged manifest
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
sync_only=0
[ "${1:-}" = "--sync" ] && { sync_only=1; shift; }
for id in "$@"; do
  python3 - "$ROOT/modules/$id/manifest.json" "$ROOT/build/modules/apk/assets/modules/$id/manifest.json" "$sync_only" <<'PY'
import os, re, sys, json
src, staged, sync_only = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
s = open(src).read()
if not sync_only:
    s = re.sub(r'("version": "[^"+]*\+plm\.)(\d+)"', lambda m: f'{m.group(1)}{int(m.group(2)) + 1}"', s, count=1)
    open(src, "w").write(s)
version = json.loads(s)["version"]
note = ""
if os.path.exists(staged):
    t = re.sub(r'("version": ")[^"]*"', lambda m: f'{m.group(1)}{version}"', open(staged).read(), count=1)
    open(staged, "w").write(t)
    note = " (staged copy updated)"
print(json.loads(s)["id"], version + note)
PY
done
