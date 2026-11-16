#!/usr/bin/env bash
# Looks a symbol up in the index (tools/dev/index_symbols.py): exact name first, else names starting with it.
#   tools/dev/sym.sh lvlSetMultipliersForDifficulty [goldeneye-pc-port]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
files="$ROOT/build/index/${2:-*}.tags"
hits=$(grep -h -P "^\Q$1\E\t" $files 2>/dev/null | head -20)
[ -z "$hits" ] && hits=$(grep -h -P "^\Q$1\E" $files 2>/dev/null | head -20)
[ -z "$hits" ] && { echo "not in the index (run tools/dev/index_symbols.py)"; exit 1; }
printf '%s\n' "$hits" | cut -c1-200
