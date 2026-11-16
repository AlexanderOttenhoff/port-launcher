# Survey data

Research material behind [docs/planning/SURVEY.md](../docs/planning/SURVEY.md) and
[LANDSCAPE.md](../docs/planning/LANDSCAPE.md).

- `repos.tsv`: surveyed repositories pinned to the commits that were read (shallow clones, 2026-11-11).
- `github-stats-2026-11-11.tsv`: stars, created, last push, license, release count, latest tag/date,
  latest-release downloads and number of Android assets, from the GitHub API.
- Curated web sources used for cross-checking are listed and rated in LANDSCAPE.md ("Curated sources").
- `sources/`: the clones themselves (about 2.3 GB, gitignored). Recreate with `./fetch.sh`.

The clones are third-party code, read for research only. Don't build or run them from here,
and never commit them.
