#!/usr/bin/env python3
"""Stages each bundled port's own icon into the APK's module assets (assets/modules/<id>/icon.png).

    tools/stage_icons.py            (tools/build_apk.sh runs it before Gradle)

Why (owner request 2026-11-13): library rows show the icon of the decomp/recomp a row plays with, so two ports of
one game (Lighthouse vs BanjoRecomp) are told apart at a glance. The icons are the ports' own art, taken from their
pinned upstream checkouts at build time and written only under build/ (gitignored), never committed: we don't hold
their copyright, and some are game-derived art (keep them out of our own branding, ROADMAP trademark rule).

Each module's source path is in ICONS below. SHORTCUT (MVP): a table here instead of a manifest field, because the
upstream checkout dirs aren't named after module ids and JFG's lives outside build/upstream. Move it to the module
manifests ("icon": path in the upstream checkout) when the module server packages modules (then the server builds
and ships the icon with the module instead).

Output: 192x192 PNG, the source scaled to fit and centred on transparency. .ico sources use their largest frame.
A missing source only warns: the library falls back to no icon.
"""
import os
import sys

# Pillow is imported only when an icon is actually staged: the APK workflow builds with no modules and has no
# Pillow installed (it failed there on 2026-11-13); the modules workflow installs it.
Image = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UP = os.path.join(ROOT, "build", "upstream")
JFG = os.path.expanduser(os.environ.get("JFG_ANDROID", "~/src/JFG-Android"))

ICONS = {
    "banjo-recomp": os.path.join(UP, "BanjoRecomp/icons/appb.ico"),
    "banjotooie-recomp": os.path.join(UP, "BanjoTooieRecompiled/assets/icon.png"),
    "blastcorps-recomp": os.path.join(UP, "blastcorps-recomp/icons/512.png"),
    "hm64-recomp": os.path.join(UP, "HarvestMoon64Recomp/icons/app.png"),
    "sk2-recomp": os.path.join(UP, "snowboardkids2-recomp/icons/512.png"),
    "bh-recomp": os.path.join(UP, "body-harvest-recomp/assets/AppIcon.ico"),
    "mm64-recomp": os.path.join(UP, "MegaMan64Recompiled/icons/512.png"),
    "bm64-recomp": os.path.join(UP, "BM64Recomp/icons/512.png"),
    "pw64-recomp": os.path.join(UP, "pilotwings-64-recomp/assets/AppIcon.ico"),
    "snap-recomp": os.path.join(UP, "Snap64Recomp/src/snap64.png"),
    "dk64-recomp": os.path.join(UP, "Donkey-Kong-64-Recompiled/icons/256.ico"),
    "ghostship": os.path.join(UP, "Ghostship/macosx/GhostshipIcon.icon/Assets/GhostshipIcon.png"),
    "goldenballoon": os.path.join(UP, "goldenballoon/brand/appicon-source.png"),
    "goldeneye": os.path.join(UP, "goldeneye-pc-port/docs/img/icon/ge007-icon.png"),
    "jfg-recomp": os.path.join(JFG, "icons/icon.ico"),
    "lighthouse": os.path.join(UP, "Lighthouse/res/lighthouse.ico"),
    "paperboat": os.path.join(UP, "PaperBoat/logo.png"),
    "perfect-dark": os.path.join(UP, "perfect_dark/dist/windows/icon.ico"),
    "shipwright": os.path.join(UP, "Shipwright/soh/macosx/sohIcon.png"),
    "spaghettikart": os.path.join(UP, "SpaghettiKart/icon.png"),
    "2ship2harkinian": os.path.join(UP, "2ship2harkinian/mm/linux/2s2hIcon.png"),
    "starship": os.path.join(UP, "Starship/starship.ico"),
    "zelda64-recomp": os.path.join(UP, "Zelda64Recomp/icons/512.ico"),
}
SIZE = 192


def load(path):
    global Image
    if Image is None:
        from PIL import Image as _Image
        Image = _Image
    img = Image.open(path)
    if path.lower().endswith(".ico"):
        # An .ico holds several sizes; take the largest instead of Pillow's default pick.
        sizes = sorted(img.info.get("sizes", {img.size}), key=lambda s: s[0] * s[1])
        if sizes:
            img.size = sizes[-1]
            img.load()
    return img.convert("RGBA")


def fit(img):
    img.thumbnail((SIZE, SIZE), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.paste(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2), img)
    return out


def main():
    stages = os.path.join(ROOT, "build", "modules")
    staged = 0
    for module, src in ICONS.items():
        # Every staging dir that bundles this module (apk, apk-decomp…) gets the icon next to its manifest.
        targets = [os.path.join(stages, s, "assets", "modules", module) for s in sorted(os.listdir(stages))
                   if os.path.isdir(os.path.join(stages, s, "assets", "modules", module))] if os.path.isdir(stages) else []
        if not targets:
            continue
        if not os.path.isfile(src):
            print(f"stage_icons: no icon for {module} ({src} missing)", file=sys.stderr)
            continue
        icon = fit(load(src))
        for t in targets:
            icon.save(os.path.join(t, "icon.png"))
            staged += 1
    print(f"stage_icons: {staged} icon(s) staged")


if __name__ == "__main__":
    main()
