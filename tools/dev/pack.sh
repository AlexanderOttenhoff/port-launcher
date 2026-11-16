#!/usr/bin/env bash
# Packages every module in modules/downloadable.txt for the local dev module server (build/modules/dist, served on
# :8765 by `python3 -m http.server 8765 --bind 0.0.0.0` from that folder). Pulled modules ("# pulled") are left out of
# catalog.json automatically. Prints one line per module.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
python3 tools/package_modules.py --base-url "${BASE_URL:-http://127.0.0.1:8765}" \
  $(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' modules/downloadable.txt) | sed 's/, sha256.*//'
