#!/usr/bin/env bash
# Builds and packages every module in modules/downloadable.txt, as the Modules workflow does (and locally, to test it).
#   BASE_URL=https://.../releases/download/<tag> tools/ci/build_modules.sh
# Output: build/modules/dist (zips, icons, index.json). The official decomp tier only: these builds read no ROM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
: "${BASE_URL:?set BASE_URL to where the release files will be served}"
# CI runners have 4 cores and 16 GB; locally the build scripts' memory cap applies (systemd-run) whatever JOBS is.
export JOBS="${JOBS:-4}"

ids=$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' modules/downloadable.txt)
# Pre-build check (owner 2026-11-16): third-party mods are linked, never bundled, so a dead link breaks the feature.
python3 tools/check_mod_links.py
[ -f build/deps/SDL2-2.32.10-android-arm64-plm/lib/libSDL2.a ] || tools/deps/build_sdl2.sh
# The libultraship family (manifest "family": "libultraship") also needs libultraship's Android dependencies; their
# prepare.sh builds a host Torch (clang + ninja on the runner) to pack the port's own .o2r.
if grep -qs '"family": "libultraship"' $(printf 'modules/%s/manifest.json ' $ids); then
  [ -d build/deps/lus-android-arm64/lib ] || tools/deps/build_lus_deps.sh
fi
for id in $ids; do
  echo "::group::$id"
  "modules/$id/prepare.sh"
  "modules/$id/build.sh"
  # On CI, drop the module's build tree once it's staged: object files with debug info filled the runner's disk
  # (2026-11-13). The staged library and data (build/modules/apk) and the upstream checkout (icons) stay.
  [ -z "${CI:-}" ] || { rm -rf "build/modules/$id/cmake"; df -h . | tail -1; }
  echo "::endgroup::"
done
python3 tools/stage_icons.py             # port icons from the fresh upstream checkouts
rm -rf build/modules/dist
python3 tools/package_modules.py --base-url "$BASE_URL" $ids
