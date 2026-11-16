#!/usr/bin/env python3
"""Adds our own settings profiles to a module's generated settings_schema.json.

    tools/schema/add_profiles.py <settings_schema.json> <module id>      (no-op without tools/schema/profiles/<id>.json)

Profiles (MENU.md) are named sets of option values the player applies in one go from the Settings tab. Where the port
ships presets, its schema generator turns them into profiles (SoH, 2S2H, GoldenEye, Golden Balloon). Owner 2026-11-16:
"our larger goal is to provide the best modern experience", so ports without presets get hand-picked ones too,
written in tools/schema/profiles/<module>.json as a list of {"id", "label", "detail", "values": {key: value}}.
They go after the port's own profiles and are just as optional: nothing changes until the player applies one.

Every value is checked against the generated schema, so a pinned-upstream bump that renames or drops an option can't
silently ship a broken profile: an unknown key, a value the option doesn't offer, or a forced key (ours, always sent)
is dropped with a warning on stderr; a profile left with no values is dropped entirely.
"""
import json
import os
import sys


def valid(option, value):
    if "values" in option:
        return any(v["value"] == value for v in option["values"])
    try:
        return float(option.get("min", 0)) <= float(value) <= float(option.get("max", 0))
    except ValueError:
        return False


def main(schema_path, module):
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles", f"{module}.json")
    if not os.path.isfile(src):
        return
    schema = json.load(open(schema_path))
    by_key = {o["key"]: o for o in schema["options"]}
    added = []
    for p in json.load(open(src)):
        kept = {}
        for key, value in p["values"].items():
            o = by_key.get(key)
            if o is None:
                print(f"{module}/{p['id']}: dropped {key} (not an option)", file=sys.stderr)
            elif o.get("force"):
                print(f"{module}/{p['id']}: dropped {key} (forced by us)", file=sys.stderr)
            elif not valid(o, str(value)):
                print(f"{module}/{p['id']}: dropped {key}={value} (not one of its values)", file=sys.stderr)
            else:
                kept[key] = str(value)
        if kept:
            added.append({"id": p["id"], "label": p["label"], "detail": p["detail"], "values": kept})
            print(f"{module}/{p['id']}: {len(kept)} values", file=sys.stderr)
    taken = {p["id"] for p in schema.get("profiles", [])}
    schema["profiles"] = schema.get("profiles", []) + [p for p in added if p["id"] not in taken]
    with open(schema_path, "w") as f:
        json.dump(schema, f, indent=1, ensure_ascii=False)
        f.write("\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <settings_schema.json> <module id>")
    main(sys.argv[1], sys.argv[2])
