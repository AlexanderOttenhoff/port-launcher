#!/usr/bin/env bash
# Builds modules with the memory cap (32 GB box, no swap: an uncapped 32-job build froze it) and prints only what
# matters: per module OK/FAILED, the errors, and warnings in files changed in the upstream checkout. Full logs in
# build/logs/<id>.log.
#   tools/dev/build_module.sh goldeneye [perfect-dark ...]      JOBS=12 by default
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build/logs
status=0
for id in "$@"; do
  log="build/logs/$id.log"
  if systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 env JOBS="${JOBS:-12}" "modules/$id/build.sh" >"$log" 2>&1; then
    echo "OK      $id"
  else
    echo "FAILED  $id   (full log: $log)"; status=1
  fi
  grep -E " error:|^ninja: build stopped|undefined reference" "$log" | head -20
  # Warnings only in files our own patches touch (commits by "plm" in the upstream checkout), not the port's own.
  for repo in $(ls "modules/$id/patches" 2>/dev/null); do
    [ -d "build/upstream/$repo/.git" ] || continue
    changed=$(git -C "build/upstream/$repo" log --author=plm --name-only --format= 2>/dev/null | sed 's|.*/||' | sort -u | paste -sd'|')
    [ -n "$changed" ] && grep -E "warning:" "$log" | grep -E "/($changed):" | grep -v "incompatible-pointer\|int-conversion" | head -10
  done
done
exit $status
