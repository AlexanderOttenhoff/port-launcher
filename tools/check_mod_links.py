#!/usr/bin/env python3
"""Checks that every third-party mod we link to still downloads (owner 2026-11-16: link, never bundle, third-party
mods, and guard against link rot in the pre-build checks). Run by tools/ci/build_modules.sh before building.

    tools/check_mod_links.py [modules/<id>/manifest.json ...]     (default: every module manifest)

What it checks, per linked entry (manifest "mods.curated"):
  - a GameBanana entry {"mod": <id>, "file": <name>}: the mod's page answers, isn't withdrawn, still offers that file,
    and the file's download link answers;
  - a plain link {"url": ...} (a GitHub release asset, any stable URL): it answers with a success status.
Downloads are probed with a 1-byte range request, so nothing big is fetched. Exit status 1 lists every broken link.
"""
import glob
import json
import sys
import urllib.request

UA = {"User-Agent": "port-launcher-link-check"}


def probe(url):
    req = urllib.request.Request(url, headers={**UA, "Range": "bytes=0-0"})
    with urllib.request.urlopen(req, timeout=30) as r:
        if r.status not in (200, 206):
            raise RuntimeError(f"HTTP {r.status}")


def check_gamebanana(entry):
    page = f"https://gamebanana.com/apiv11/Mod/{entry['mod']}/ProfilePage"
    with urllib.request.urlopen(urllib.request.Request(page, headers=UA), timeout=30) as r:
        profile = json.load(r)
    if profile.get("_bIsTrashed") or profile.get("_bIsWithheld") or profile.get("_bIsObsolete"):
        raise RuntimeError("the mod was withdrawn or marked obsolete")
    files = {f.get("_sFile"): f for f in profile.get("_aFiles") or []}
    wanted = entry.get("file")
    if wanted and wanted not in files:
        raise RuntimeError(f"file {wanted!r} is gone (now: {', '.join(files) or 'none'})")
    chosen = files[wanted] if wanted else next(iter(files.values()), None)
    if chosen is None:
        raise RuntimeError("the mod has no files")
    probe(chosen["_sDownloadUrl"])


def entries(manifest):
    # Curated (linked) mods: manifest "mods.curated" (owner 2026-11-16: link, never bundle, third-party mods).
    for entry in (manifest.get("mods") or {}).get("curated") or []:
        yield entry


def main(paths):
    broken = []
    count = 0
    for path in paths or sorted(glob.glob("modules/*/manifest.json")):
        with open(path) as f:
            manifest = json.load(f)
        for entry in entries(manifest):
            count += 1
            label = f"{manifest.get('id')}: {entry.get('name') or entry.get('url') or entry.get('mod')}"
            try:
                if "url" in entry:
                    probe(entry["url"])
                elif "mod" in entry:
                    check_gamebanana(entry)
                else:
                    raise RuntimeError("entry has neither \"url\" nor \"mod\"")
                print(f"ok      {label}")
            except Exception as e:  # any failure is a broken link
                broken.append(label)
                print(f"BROKEN  {label}: {e}")
    print(f"{count} linked mod(s) checked, {len(broken)} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
