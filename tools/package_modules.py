#!/usr/bin/env python3
"""Packages downloadable modules for the module server: one zip per module plus index.json.

    tools/package_modules.py --base-url URL [--out build/modules/dist] [ids...]

With no ids it packages every module listed in modules/downloadable.txt. Input is what each module's build.sh
staged (build/modules/apk: assets/modules/<id>/ and jniLibs/arm64-v8a/libplm_<id>.so); the APK build leaves the
downloadable ones out (android/app/build.gradle.kts reads the same list).

Why this layout (2026-11-13, first module-server slice, docs/planning/ROADMAP.md "Path to pre-alpha"):
- Zip `<id>-<version>-android-arm64.zip`: manifest.json, icon.png, lib/arm64-v8a/libplm_<id>.so, data/...
  The app (ModuleRepository.kt) unpacks it into files/installed-modules/<id>/ and loads the .so from there.
- index.json embeds each module's whole manifest, so the library can show a module (title, games, family, badges)
  before it's downloaded, and the core's game DB can identify the player's ROM for it.
- `sha256` and `size` let the app verify the download. Signatures (minisign) come later.
- The icon is also published on its own (`<id>.png`) so not-yet-downloaded rows can show it.
- Libraries must be 16 KB aligned (on-device spike 1: 16 KB-page devices refuse 4 KB-aligned ones), so this
  script refuses to package one that isn't.
Only modules whose build reads no ROM and contains no ROM-derived code may be packaged (the official decomp tier).
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(ROOT, "build", "modules", "apk")


def downloadable():
    with open(os.path.join(ROOT, "modules", "downloadable.txt")) as f:
        return [l.split("#")[0].strip() for l in f if l.split("#")[0].strip()]


def min_load_alignment(path):
    """Smallest p_align of the ELF's PT_LOAD segments (64-bit little-endian only, which is all we build)."""
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:4] != b"\x7fELF" or head[4] != 2:
            raise SystemExit(f"{path}: not a 64-bit ELF")
        phoff, = struct.unpack_from("<Q", head, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", head, 0x36)
        aligns = []
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ph = f.read(phentsize)
            p_type, = struct.unpack_from("<I", ph, 0)
            if p_type == 1:  # PT_LOAD
                aligns.append(struct.unpack_from("<Q", ph, 48)[0])
        return min(aligns) if aligns else 0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stripped_copy(lib):
    """The library without debug info, in a temp file (None if no llvm-strip is found: the unstripped one ships).

    Why: the libultraship modules build RelWithDebInfo (symbols help with device crash logs), which makes their .so
    ~150 MB; the download only needs the code. Gradle strips the APK's bundled copies the same way. The staged file
    stays unstripped for local debugging (ndk-stack against it)."""
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.path.join(
        os.environ.get("ANDROID_HOME", os.path.expanduser("~/Android/Sdk")), "ndk", "26.0.10792818")
    strip = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-strip")
    if not os.path.exists(strip):
        print(f"package_modules: no {strip}; packaging unstripped", file=sys.stderr)
        return None
    fd, out = tempfile.mkstemp(suffix=".so")
    os.close(fd)
    subprocess.run([strip, "--strip-debug", "-o", out, lib], check=True)
    return out


def package(module_id, out_dir, base_url):
    pkg = os.path.join(STAGE, "assets", "modules", module_id)
    lib = os.path.join(STAGE, "jniLibs", "arm64-v8a", f"libplm_{module_id}.so")
    manifest_path = os.path.join(pkg, "manifest.json")
    for p in (manifest_path, lib):
        if not os.path.exists(p):
            raise SystemExit(f"{module_id}: {p} missing (run modules/{module_id}/build.sh first)")
    manifest = json.load(open(manifest_path))
    if "@UPSTREAM_COMMIT@" in json.dumps(manifest):
        raise SystemExit(f"{module_id}: manifest still has the @UPSTREAM_COMMIT@ placeholder (not a staged build)")
    if manifest.get("family") == "n64recomp":
        raise SystemExit(f"{module_id}: recomp builds contain ROM-derived code and are never published")
    align = min_load_alignment(lib)
    if align < 16384:
        raise SystemExit(f"{module_id}: libplm_{module_id}.so is {align}-byte aligned; 16 KB-page devices need 16384")
    version = manifest.get("version", "0")
    # GitHub renames some characters in release file names ("+" among them), which would break the URL in the
    # index; keep file names to [A-Za-z0-9._-].
    safe_version = "".join(c if c.isalnum() or c in "._-" else "_" for c in version)
    name = f"{module_id}-{safe_version}-android-arm64.zip"
    zpath = os.path.join(out_dir, name)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(manifest_path, "manifest.json")
        icon = os.path.join(pkg, "icon.png")
        if os.path.exists(icon):
            z.write(icon, "icon.png")
            shutil.copy(icon, os.path.join(out_dir, f"{module_id}.png"))
        small = stripped_copy(lib)
        z.write(small or lib, f"lib/arm64-v8a/libplm_{module_id}.so")
        if small:
            os.remove(small)
        data = os.path.join(pkg, "data")
        for root, _, files in os.walk(data):
            for fn in sorted(files):
                full = os.path.join(root, fn)
                z.write(full, os.path.relpath(full, pkg))
    entry = {
        "id": module_id,
        "version": version,
        "abi": manifest.get("abi", 0),
        "platform": "android-arm64",
        "url": f"{base_url}/{name}",
        "icon": f"{base_url}/{module_id}.png" if os.path.exists(os.path.join(pkg, "icon.png")) else "",
        "size": os.path.getsize(zpath),
        "sha256": sha256(zpath),
        "manifest": manifest,
    }
    print(f"{name}: {entry['size'] / 1e6:.1f} MB, sha256 {entry['sha256'][:12]}…")
    return entry


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", required=True, help="where the zips will be served, e.g. a GitHub release download URL")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "modules", "dist"))
    ap.add_argument("ids", nargs="*")
    args = ap.parse_args()
    ids = args.ids or downloadable()
    os.makedirs(args.out, exist_ok=True)
    entries = [package(i, args.out, args.base_url.rstrip("/")) for i in ids]
    index = {"format": 1, "modules": entries}
    with open(os.path.join(args.out, "index.json"), "w") as f:
        json.dump(index, f, indent=1)
    print(f"index.json: {len(entries)} module(s) in {args.out}")
    # catalog.json: the list the app shows (ModuleRepository). The real one lives on the modules repository's default
    # branch and is edited by hand to introduce modules one by one; this copy lists everything packaged, so a local
    # server (files/module-index-url override) shows all of it.
    # Modules marked "# pulled" in downloadable.txt stay out of it, as on the real one (no hand-trimming).
    pulled = pulled_ids()
    with open(os.path.join(args.out, "catalog.json"), "w") as f:
        json.dump({"format": 1, "modules": [{"id": e["id"]} for e in entries if e["id"] not in pulled]}, f, indent=2)
        f.write("\n")


def pulled_ids():
    """Ids whose downloadable.txt line ends in "# pulled": built and packaged, not listed in catalog.json."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "modules", "downloadable.txt")
    out = set()
    with open(path) as f:
        for line in f:
            body, _, comment = line.partition("#")
            if body.strip() and comment.strip().lower().startswith("pulled"):
                out.add(body.strip())
    return out


if __name__ == "__main__":
    sys.exit(main())
