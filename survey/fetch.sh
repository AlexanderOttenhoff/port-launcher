#!/usr/bin/env bash
# Recreate survey/sources/ from repos.tsv at the recorded commits (read-only research clones).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p sources
tail -n +2 repos.tsv | while IFS=$'\t' read -r dir repo commit _date; do
  if [ -d "sources/$dir/.git" ]; then echo "skip $dir"; continue; fi
  git init -q "sources/$dir"
  git -C "sources/$dir" remote add origin "https://github.com/$repo.git"
  git -C "sources/$dir" fetch -q --depth 1 origin "$commit"
  git -C "sources/$dir" checkout -q FETCH_HEAD
  echo "ok   $dir"
done
