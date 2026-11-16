#!/usr/bin/env python3
"""Emulator/device boot check from the library UI (the GAME_STATUS "Emulator" column).

    tools/test/boot_check.py [--serial emulator-5554] [--seconds 40] "Harvest Moon 64" "Blast Corps" ...

For each title: (re)start the launcher, find the title's row in the library via the UI hierarchy (uiautomator),
scrolling as needed, tap it, take "Launch game" on the game's page, wait, and summarise logcat:
ROM accepted, host first frame, game-code load, fatal signals. Verdicts are from logs only: recomps and libultraship
ports often draw nothing in the emulator (ARM translation, software GPU).
"""
import argparse, re, subprocess, time

def adb(serial, *args, check=False):
    return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True, check=check).stdout

def dump(serial):
    adb(serial, "shell", "uiautomator", "dump", "/sdcard/plm-ui.xml")
    return adb(serial, "shell", "cat", "/sdcard/plm-ui.xml")

def find(xml, text, prefix=False):
    for m in re.finditer(r'text="([^"]*)"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
        if m.group(1) == text or (prefix and m.group(1).startswith(text)):
            x1, y1, x2, y2 = map(int, m.groups()[1:])
            return (x1 + x2) // 2, (y1 + y2) // 2
    return None

def screen_height(serial):
    m = re.search(r"(\d+)x(\d+)", adb(serial, "shell", "wm", "size"))
    if not m:
        return 2400
    w, h = int(m.group(1)), int(m.group(2))
    rot = adb(serial, "shell", "settings", "get", "system", "user_rotation").strip()
    return w if rot in ("1", "3") else h  # landscape (as the RP6): the physical width is the visible height

def open_title(serial, title):
    height = screen_height(serial)
    adb(serial, "shell", "am", "force-stop", "app.portlauncher")
    adb(serial, "shell", "monkey", "-p", "app.portlauncher", "-c", "android.intent.category.LAUNCHER", "1")
    time.sleep(5)
    for _ in range(12):
        pos = find(dump(serial), title)
        if pos and pos[1] < height * 0.9:
            adb(serial, "shell", "input", "tap", str(pos[0]), str(pos[1]))
            time.sleep(2.5)
            ui = dump(serial)
            # "Update (8 MB)" carries the size, hence the prefix match ("Update available…" is the message, not the button).
            dl = find(ui, "Download") or find(ui, "Update (", prefix=True)
            if dl:  # a module-server module: download (or update) it, then select the row again
                adb(serial, "shell", "input", "tap", str(dl[0]), str(dl[1]))
                # Wait for the download to finish: the row stays listed meanwhile (disabled), so waiting for the title
                # alone tapped it mid-download (2026-11-15).
                time.sleep(3)
                for _ in range(90):
                    ui = dump(serial)
                    if find(ui, title) and not find(ui, "Downloading", prefix=True):
                        break
                    time.sleep(2)
                pos = find(dump(serial), title)
                if pos:
                    adb(serial, "shell", "input", "tap", str(pos[0]), str(pos[1]))
                    time.sleep(2.5)
            # Selecting a game opens its page (2026-11-15); "Launch game" there starts it.
            play = find(dump(serial), "Launch game")
            if play:
                adb(serial, "shell", "input", "tap", str(play[0]), str(play[1]))
            return True
        adb(serial, "shell", "input", "swipe", "540", str(int(height * 0.8)), "540", str(int(height * 0.35)), "400")
        time.sleep(1)
    return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default="emulator-5554")
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("titles", nargs="+")
    a = ap.parse_args()
    for title in a.titles:
        adb(a.serial, "logcat", "-c")
        if not open_title(a.serial, title):
            print(f"{title}: NOT FOUND in the library"); continue
        time.sleep(a.seconds)
        log = adb(a.serial, "logcat", "-d")
        pid = adb(a.serial, "shell", "pidof", "app.portlauncher:game").strip()
        rom = re.search(r"--rom .*?: (accepted|rejected[^\n]*)", log)
        first = re.search(r"plm-host: first frame (\d+) ms", log)
        fatal = re.findall(r"(Fatal signal \d+ [^\n]*|FATAL EXCEPTION[^\n]*|Abort message: [^\n]*)", log)
        lib = "game code loaded" in log
        print(f"{title}: rom={'%s' % rom.group(1) if rom else '?'} first_frame={first.group(1) + 'ms' if first else 'no'} "
              f"running={'yes' if pid else 'no'}{' gamelib=loaded' if lib else ''}"
              f"{' FATAL: ' + fatal[0][:140] if fatal else ''}")
        adb(a.serial, "shell", "am", "force-stop", "app.portlauncher")

if __name__ == "__main__":
    main()
