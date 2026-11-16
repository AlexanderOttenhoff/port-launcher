#!/usr/bin/env python3
"""Validates the intent library data (docs/planning/intents/FORMAT.md; INPUT_INTENTS.md step 2).

    tools/intents/validate.py [--root docs/planning/intents] [--show MODULE[:PROFILE[:PRESET]]]

Two passes:
1. **Schema:** every profiles/sets/modules file against schema/intents.schema.json. Uses the `jsonschema` package
   when it is installed; otherwise a small built-in validator for the subset of JSON Schema the file uses (the build
   machine has no jsonschema, and this must run anywhere with a bare python3, like the other tools/ scripts).
2. **Cross-references** (what JSON Schema can't express):
   - intent ids are unique across the whole library (one id = one meaning; sets reuse others' intents by "include");
   - sets, includes, overrides, contexts and implementations name intents that exist;
   - every intent a module exposes in a context has an implementation, and every implementation is used;
   - a hold intent offering a hold/toggle choice says how the toggle is done (toggle_via);
   - every layout (each profile, with and without each preset, sets on their own and each module fully resolved)
     binds every non-optional intent of each context exactly once, without two intents on the same control, unless
     the entry is marked dual (several controls for one intent, e.g. MK8DX's items on LB and LT);
   - reserved controls (Back: the launcher menu) are never bound;
   - controls suit the intent kind (a stick intent needs a stick, a press can't sit on a whole stick);
   - contexts the game tells apart by itself ("detection": game) can't make one control mean two different port
     inputs: the layer doesn't know which context it's in, so it would have to send both. A target marked
     "scoped" (the port only reads it in its own context, like PD's UI cancel bit) is exempt.

Why the rules look like this (maximalist "why", MVP rule): the runtime (step 3) and the controls page (step 5) will
read these files as-is, so every mistake caught here is a mis-bound button not shipped. The checks are the contract
the runtime relies on: a resolved layout is a total, conflict-free function from (context, control, gesture,
modifiers) to one intent.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "planning", "intents"))

errors = []
warnings = []


def err(where, msg):
    errors.append(f"{where}: {msg}")


def warn(where, msg):
    warnings.append(f"{where}: {msg}")


# ---------------------------------------------------------------------------------------------------------------
# Pass 1: schema
# ---------------------------------------------------------------------------------------------------------------

class MiniSchema:
    """The JSON Schema subset intents.schema.json uses: type, enum, const, pattern, minLength, minimum, properties,
    required, additionalProperties, propertyNames, minProperties, items, minItems, uniqueItems, oneOf, anyOf, $ref
    (local JSON pointers). Enough to give a useful error path; not a general implementation."""

    TYPES = {
        "object": dict, "array": list, "string": str, "boolean": bool, "null": type(None),
    }

    def __init__(self, root):
        self.root = root

    def resolve(self, ref):
        if not ref.startswith("#/"):
            raise ValueError(f"only local $ref supported: {ref}")
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part.replace("~1", "/").replace("~0", "~")]
        return node

    def errors(self, value, schema, path="$"):
        out = []
        if "$ref" in schema:
            out += self.errors(value, self.resolve(schema["$ref"]), path)
        t = schema.get("type")
        if t is not None:
            if t == "integer":
                ok = isinstance(value, int) and not isinstance(value, bool)
            elif t == "number":
                ok = isinstance(value, (int, float)) and not isinstance(value, bool)
            else:
                ok = isinstance(value, self.TYPES[t]) and not (t != "boolean" and isinstance(value, bool))
            if not ok:
                return out + [f"{path}: expected {t}, got {type(value).__name__}"]
        if "const" in schema and value != schema["const"]:
            out.append(f"{path}: must be {schema['const']!r}")
        if "enum" in schema and value not in schema["enum"]:
            out.append(f"{path}: {value!r} not one of {schema['enum']}")
        if isinstance(value, str):
            if "pattern" in schema and not re.search(schema["pattern"], value):
                out.append(f"{path}: {value!r} doesn't match {schema['pattern']}")
            if len(value) < schema.get("minLength", 0):
                out.append(f"{path}: shorter than {schema['minLength']}")
        if isinstance(value, (int, float)) and not isinstance(value, bool) and "minimum" in schema:
            if value < schema["minimum"]:
                out.append(f"{path}: below {schema['minimum']}")
        if isinstance(value, dict):
            props = schema.get("properties", {})
            for r in schema.get("required", []):
                if r not in value:
                    out.append(f"{path}: missing {r!r}")
            if len(value) < schema.get("minProperties", 0):
                out.append(f"{path}: needs at least {schema['minProperties']} properties")
            for k, v in value.items():
                if "propertyNames" in schema:
                    out += [e for e in self.errors(k, schema["propertyNames"], f"{path}.<key {k}>")]
                if k in props:
                    out += self.errors(v, props[k], f"{path}.{k}")
                else:
                    ap = schema.get("additionalProperties", True)
                    if ap is False:
                        out.append(f"{path}: unexpected property {k!r}")
                    elif isinstance(ap, dict):
                        out += self.errors(v, ap, f"{path}.{k}")
        if isinstance(value, list):
            if len(value) < schema.get("minItems", 0):
                out.append(f"{path}: needs at least {schema['minItems']} items")
            if schema.get("uniqueItems"):
                seen = [json.dumps(v, sort_keys=True) for v in value]
                if len(seen) != len(set(seen)):
                    out.append(f"{path}: items not unique")
            if "items" in schema:
                for i, v in enumerate(value):
                    out += self.errors(v, schema["items"], f"{path}[{i}]")
        for key in ("oneOf", "anyOf"):
            if key in schema:
                results = [self.errors(value, s, path) for s in schema[key]]
                passing = sum(1 for r in results if not r)
                if key == "oneOf" and passing != 1 or key == "anyOf" and passing == 0:
                    if passing > 1:
                        out.append(f"{path}: matches {passing} alternatives of oneOf")
                    else:
                        # Report the alternative that got furthest (fewest errors): usually the intended one.
                        best = min(results, key=len)
                        out += best
        return out


def schema_check(schema, docs):
    try:
        import jsonschema  # noqa: F401
        validator = jsonschema.Draft202012Validator(schema)
        for name, doc in docs:
            for e in sorted(validator.iter_errors(doc), key=lambda e: list(e.path)):
                err(name, f"schema: {'/'.join(map(str, e.path)) or '$'}: {e.message}")
    except ImportError:
        mini = MiniSchema(schema)
        for name, doc in docs:
            for e in mini.errors(doc, schema):
                err(name, f"schema: {e}")


# ---------------------------------------------------------------------------------------------------------------
# Pass 2: cross-references
# ---------------------------------------------------------------------------------------------------------------

class Library:
    def __init__(self, profiles, sets):
        self.profiles_doc = profiles
        self.profile_ids = [p["id"] for p in profiles["profiles"]]
        self.face = {p["id"]: p["face_positions"] for p in profiles["profiles"]}
        c = profiles["controls"]
        self.buttons = set(c["buttons"])
        self.axes = set(c["axes"])
        self.sticks = set(c["sticks"])
        self.directions = set(c["directions"])
        self.reserved = set(profiles.get("reserved", {}))
        self.sets = {s["id"]: s for s in sets}
        self.intent_home = {}   # intent id -> set id
        self.intents = {}       # intent id -> definition

    def index(self):
        for sid, s in self.sets.items():
            for i in s["intents"]:
                if i["id"] in self.intent_home:
                    err(f"sets/{sid}", f"intent {i['id']!r} already defined in set {self.intent_home[i['id']]!r}; "
                        "include it instead of redefining")
                    continue
                self.intent_home[i["id"]] = sid
                self.intents[i["id"]] = i
        for sid, s in self.sets.items():
            for inc in s.get("include", []):
                if inc not in self.intents:
                    err(f"sets/{sid}", f"include {inc!r}: no set defines that intent")
                elif self.intent_home[inc] == sid:
                    warn(f"sets/{sid}", f"include {inc!r}: already defined here")

    def set_universe(self, sid):
        s = self.sets[sid]
        return [i["id"] for i in s["intents"]] + [i for i in s.get("include", []) if i in self.intents]

    # -- layouts ---------------------------------------------------------------------------------------------

    def map_control(self, control, src, dst, by):
        if by == "label":
            return control
        if control in ("a", "b", "x", "y"):
            pos = self.face[src][control]
            return next(lbl for lbl, p in self.face[dst].items() if p == pos)
        return control

    def map_binding(self, b, src, dst, by):
        if b is None:
            return None
        if isinstance(b, str):
            return self.map_control(b, src, dst, by)
        if "bind" in b:
            return {"bind": [self.map_binding(x, src, dst, by) for x in b["bind"]], "dual": True}
        out = dict(b)
        out["control"] = self.map_control(b["control"], src, dst, by)
        if "with" in b:
            out["with"] = [self.map_control(w, src, dst, by) for w in b["with"]]
        return out

    def resolve_layer(self, layouts, profile, where, stack=()):
        """A document's own layer for one profile, with derive_from applied (derivation only ever reads the same
        document's layers, so a set's or core's per-profile choices survive into modules: FORMAT.md)."""
        if profile not in layouts:
            return None
        if profile in stack:
            err(where, f"derive_from cycle through {profile!r}")
            return {"bindings": {}, "contexts": {}}
        layer = layouts[profile]
        out = {"bindings": {}, "contexts": {}}
        src = layer.get("derive_from")
        if src is not None:
            if src not in self.profile_ids:
                err(where, f"layout {profile!r}: derive_from unknown profile {src!r}")
            base = self.resolve_layer(layouts, src, where, stack + (profile,))
            if base is None:
                err(where, f"layout {profile!r}: derive_from {src!r}, which this document doesn't define")
            else:
                by = layer.get("by", "position")
                out["bindings"] = {k: self.map_binding(v, src, profile, by) for k, v in base["bindings"].items()}
                out["contexts"] = {c: {k: self.map_binding(v, src, profile, by) for k, v in m.items()}
                                   for c, m in base["contexts"].items()}
        elif "by" in layer:
            warn(where, f"layout {profile!r}: 'by' without derive_from is ignored")
        out["bindings"].update(layer.get("bindings", {}))
        for c, m in layer.get("contexts", {}).items():
            out["contexts"].setdefault(c, {}).update(m)
        return out


def merge(dst, layer):
    if layer is None:
        return
    dst["bindings"].update(layer["bindings"])
    for c, m in layer["contexts"].items():
        dst["contexts"].setdefault(c, {}).update(m)


def flatten(entry):
    """A binding entry -> list of normalised bindings (control, gesture, frozenset(with))."""
    if entry is None:
        return []
    items = entry["bind"] if isinstance(entry, dict) and "bind" in entry else [entry]
    out = []
    for b in items:
        if isinstance(b, str):
            out.append((b, "press", frozenset()))
        else:
            out.append((b["control"], b.get("gesture", "press"), frozenset(b.get("with", []))))
    return out


def parts(control):
    """The physical pieces a control occupies: a whole stick overlaps its four directions."""
    for stick in ("left_stick", "right_stick"):
        if control == stick:
            return {f"{stick}_{d}" for d in ("up", "down", "left", "right")}
    if control == "dpad":
        return {f"dpad_{d}" for d in ("up", "down", "left", "right")}
    return {control}


def gestures_conflict(g1, g2):
    """Two bindings on the same control conflict unless the gestures can be told apart: tap vs long press, and a
    double tap beside anything else (the flight set's bank on LB with a double-tapped LB barrel roll is Star Fox's
    own grammar). A plain press fires on every touch, so it can't share with tap or long press."""
    if g1 == g2:
        return True
    if "double_tap" in (g1, g2):
        return False
    return "press" in (g1, g2)


def bindings_conflict(a, b):
    return a[2] == b[2] and parts(a[0]) & parts(b[0]) and gestures_conflict(a[1], b[1])


def kind_check(lib, where, intent_id, kind, binding):
    control, gesture, mods = binding
    for m in mods:
        if m not in lib.buttons:
            err(where, f"{intent_id}: modifier {m!r} must be a button")
    if kind == "stick":
        if control not in lib.sticks:
            err(where, f"{intent_id}: a stick intent needs a stick (left_stick, right_stick, dpad), got {control!r}")
    elif kind == "axis":
        if control not in lib.axes | lib.buttons | lib.directions:
            err(where, f"{intent_id}: an axis intent needs a trigger, a button or a stick direction, got {control!r}")
    else:
        if control in lib.sticks:
            err(where, f"{intent_id}: a {kind} intent can't take a whole stick ({control!r}); use a direction")
    if gesture in ("tap", "long_press", "double_tap") and kind in ("axis", "stick"):
        err(where, f"{intent_id}: gesture {gesture!r} on an {kind} intent")
    if control in lib.reserved or mods & lib.reserved:
        err(where, f"{intent_id}: {control!r} is reserved ({lib.profiles_doc['reserved'].get(control, '')})")


def check_layout(lib, where, contexts, layout, intents, universe, context_ids):
    """contexts: {context id: [intent ids]} to check. intents: id -> effective definition."""
    for k in layout["bindings"]:
        if k not in universe:
            err(where, f"binding for unknown intent {k!r}")
    for c, m in layout["contexts"].items():
        if c not in context_ids:
            err(where, f"bindings for unknown context {c!r}")
            continue
        for k in m:
            if k not in universe:
                err(where, f"context {c!r}: binding for unknown intent {k!r}")
            elif c in contexts and k not in contexts[c]:
                warn(where, f"context {c!r}: binding for {k!r}, which the context doesn't expose")
    effective = {}
    for c, ids in contexts.items():
        ctx_over = layout["contexts"].get(c, {})
        bound = []
        for i in ids:
            entry = ctx_over[i] if i in ctx_over else layout["bindings"].get(i)
            d = intents[i]
            flat = flatten(entry)
            if not flat:
                if not d.get("optional"):
                    err(where, f"context {c!r}: {i!r} ({d['label']}) is not bound")
                continue
            for b in flat:
                kind_check(lib, f"{where} context {c!r}", i, d["kind"], b)
                for j, other in bound:
                    if bindings_conflict(b, other):
                        mods = "+".join(sorted(b[2]))
                        err(where, f"context {c!r}: {i!r} and {j!r} both on {b[0]}"
                            f"{' with ' + mods if mods else ''} ({b[1]}/{other[1]})")
                bound.append((i, b))
            effective.setdefault(c, {})[i] = flat
    return effective


# ---------------------------------------------------------------------------------------------------------------

def check_set(lib, s):
    where = f"sets/{s['id']}"
    universe = lib.set_universe(s["id"])
    intents = {i: lib.intents[i] for i in universe}
    for i in s["intents"]:
        check_intent_def(f"{where} intent {i['id']!r}", i)
    for p in lib.profile_ids:
        layer = lib.resolve_layer(s["layouts"], p, where)
        if layer is None:
            err(where, f"no layout for profile {p!r} (sets cover every profile; derive_from keeps it short)")
            continue
        if layer["contexts"]:
            err(where, f"layout {p!r}: sets have no contexts; context bindings belong in modules")
        check_layout(lib, f"{where} [{p}]", {"(set)": universe}, layer, intents, universe, {"(set)"})
        for name, preset in s.get("presets", {}).items():
            pl = lib.resolve_layer(preset["layouts"], p, f"{where} preset {name!r}")
            if pl is None:
                err(where, f"preset {name!r}: no layout for profile {p!r}")
                continue
            full = {"bindings": dict(layer["bindings"]), "contexts": {}}
            merge(full, pl)
            check_layout(lib, f"{where} [{p} + {name}]", {"(set)": universe}, full, intents, universe, {"(set)"})


def check_intent_def(where, d):
    hm = d.get("hold_mode")
    if hm and d["kind"] not in ("hold", "toggle"):
        err(where, f"hold_mode on a {d['kind']} intent (only hold/toggle intents have one)")
    if d["kind"] in ("axis", "stick") and "threshold" in d.get("preferences", []) and d["kind"] == "stick":
        warn(where, "threshold preference on a stick")


def target_signature(impl):
    """What a translated intent sends, for the game-context consistency check; None = exempt."""
    if impl["mode"] == "native":
        impl = impl.get("fallback")
        if impl is None:
            return None
    if impl["mode"] != "translated":
        return None
    t = impl["target"]
    if t.get("scoped"):
        return None
    if "n64" in t:
        return "n64:" + "+".join(t["n64"])
    return "port:" + t.get("port", "?")


def impl_signature(impl):
    """The port input an implementation sends, scoped or not (None: injected, or unknown)."""
    if impl is None:
        return None
    if impl["mode"] == "native":
        impl = impl.get("fallback")
        if impl is None:
            return None
    t = impl.get("target")
    if not t:
        return None
    return "n64:" + "+".join(t["n64"]) if "n64" in t else "port:" + t.get("port", "?")


def resolve_module(lib, m, profile, preset=None):
    layout = {"bindings": {}, "contexts": {}}
    for sid in m["sets"]:
        if sid in lib.sets:
            merge(layout, lib.resolve_layer(lib.sets[sid]["layouts"], profile, f"sets/{sid}"))
    merge(layout, lib.resolve_layer(m.get("layouts", {}), profile, f"modules/{m['module']}"))
    if preset:
        for sid in m["sets"]:
            sp = lib.sets.get(sid, {}).get("presets", {}).get(preset)
            if sp:
                merge(layout, lib.resolve_layer(sp["layouts"], profile, f"sets/{sid} preset {preset!r}"))
        mp = m.get("presets", {}).get(preset)
        if mp:  # None = the module hides that set preset; resolve_module isn't called for it
            pl = lib.resolve_layer(mp["layouts"], profile, f"modules/{m['module']} preset {preset!r}")
            if pl is None:
                err(f"modules/{m['module']}", f"preset {preset!r}: no layout for profile {profile!r}")
            merge(layout, pl)
    return layout


def module_contexts(m, universe, where):
    """Context id -> resolved intent list (extends applied), plus the concrete (non-abstract) ones."""
    by_id = {}
    for c in m["contexts"]:
        if c["id"] in by_id:
            err(where, f"context {c['id']!r} declared twice")
        by_id[c["id"]] = c
    resolved = {}

    def resolve(cid, stack=()):
        if cid in resolved:
            return resolved[cid]
        if cid in stack:
            err(where, f"context extends cycle through {cid!r}")
            return []
        c = by_id[cid]
        out = []
        for parent in c.get("extends", []):
            if parent not in by_id:
                err(where, f"context {cid!r} extends unknown context {parent!r}")
                continue
            out += [i for i in resolve(parent, stack + (cid,)) if i not in out]
        for i in c.get("intents", []):
            if i not in universe:
                err(where, f"context {cid!r}: unknown intent {i!r} (not in the module's sets or its own intents)")
            elif i not in out:
                out.append(i)
        for i in c.get("remove", []):
            if i not in out:
                warn(where, f"context {cid!r}: remove {i!r}, which it doesn't have")
            out = [x for x in out if x != i]
        for i in c.get("labels", {}):
            if i not in out:
                err(where, f"context {cid!r}: label for {i!r}, which the context doesn't expose")
        resolved[cid] = out
        return out

    for cid in by_id:
        resolve(cid)
    concrete = {cid: ids for cid, ids in resolved.items() if not by_id[cid].get("abstract")}
    for cid, ids in concrete.items():
        if not ids:
            err(where, f"context {cid!r} exposes no intents")
    return by_id, concrete


def check_module(lib, m):
    where = f"modules/{m['module']}"
    for sid in m["sets"]:
        if sid not in lib.sets:
            err(where, f"unknown set {sid!r}")
    if "core" not in m["sets"]:
        warn(where, "doesn't use the core set (pause, menus)")
    universe = []
    for sid in m["sets"]:
        if sid in lib.sets:
            universe += [i for i in lib.set_universe(sid) if i not in universe]
    own = {}
    for d in m.get("intents", []):
        check_intent_def(f"{where} intent {d['id']!r}", d)
        if d["id"] in lib.intents:
            err(where, f"intent {d['id']!r} is a library intent (set {lib.intent_home[d['id']]!r}); "
                "use overrides to adjust it")
        elif d["id"] in own:
            err(where, f"intent {d['id']!r} declared twice")
        own[d["id"]] = d
        universe.append(d["id"])
    intents = {i: dict(lib.intents.get(i) or own[i]) for i in universe}
    for i, o in m.get("overrides", {}).items():
        if i not in intents:
            err(where, f"override for unknown intent {i!r}")
            continue
        if i in own:
            warn(where, f"override for its own intent {i!r}: edit the declaration instead")
        intents[i].update(o)
        check_intent_def(f"{where} override {i!r}", intents[i])

    by_id, contexts = module_contexts(m, universe, where)
    used = []
    for ids in contexts.values():
        used += [i for i in ids if i not in used]

    impl = m["implementation"]
    for i in used:
        if i not in impl:
            err(where, f"intent {i!r} is exposed in a context but has no implementation")
    for i, im in impl.items():
        if i not in intents:
            err(where, f"implementation for unknown intent {i!r}")
            continue
        if i not in used:
            err(where, f"implementation for {i!r}, which no context exposes")
        d = intents[i]
        hm = d.get("hold_mode")
        if hm and hm["user_choice"] and "toggle_via" not in im:
            err(where, f"{i!r} offers hold/toggle but its implementation has no toggle_via")
        if "toggle_via" in im and not (hm and hm["user_choice"]):
            warn(where, f"{i!r}: toggle_via, but the intent offers no hold/toggle choice")
        if im["mode"] == "native" and im["status"] == "planned" and "fallback" not in im:
            err(where, f"{i!r}: a planned native hook needs a translated fallback until it exists")
    for i in universe:
        if i not in used and i in own:
            warn(where, f"own intent {i!r} is in no context")

    presets = set(m.get("presets", {}))
    for sid in m["sets"]:
        presets |= set(lib.sets.get(sid, {}).get("presets", {}))
    for name, mp in m.get("presets", {}).items():
        if mp is None:
            presets.discard(name)
            if not any(name in lib.sets.get(sid, {}).get("presets", {}) for sid in m["sets"]):
                warn(where, f"preset {name!r}: hidden, but none of the module's sets has it")
    game_contexts = [c for c in contexts if by_id[c]["detection"]["by"] == "game"]
    for p in lib.profile_ids:
        for preset in [None] + sorted(presets):
            tag = f"{where} [{p}{' + ' + preset if preset else ''}]"
            layout = resolve_module(lib, m, p, preset)
            eff = check_layout(lib, tag, contexts, layout, intents, universe, set(by_id))
            # A double tap beside a press on the same button is only the game's own grammar (Star Fox: double-tap
            # the bank to roll) when both send the same port input; otherwise every double tap fires the other
            # intent twice.
            for c, by_intent in eff.items():
                for i, flat in by_intent.items():
                    for b in flat:
                        if b[1] != "double_tap":
                            continue
                        for j, other in by_intent.items():
                            if j == i:
                                continue
                            for o in other:
                                if o[1] != "double_tap" and o[2] == b[2] and parts(o[0]) & parts(b[0]):
                                    si = impl_signature(impl.get(i))
                                    sj = impl_signature(impl.get(j))
                                    if si is None or si != sj:
                                        err(tag, f"context {c!r}: double tap of {b[0]} for {i!r} also presses "
                                            f"{j!r} ({sj} vs {si}); give {i!r} its own control")
            # One control, one port input across the contexts only the game can tell apart.
            seen = {}
            for c in game_contexts:
                for i, flat in eff.get(c, {}).items():
                    if i not in impl:
                        continue
                    sig = target_signature(impl[i])
                    if sig is None:
                        continue
                    for b in flat:
                        # By physical part, so a whole D-pad in one context meets a D-pad direction in another.
                        # Tap and long press are told apart by the layer itself, so they keep their own keys.
                        for part in parts(b[0]):
                            key = (part, b[1], b[2])
                            seen.setdefault(key, {}).setdefault(sig, []).append(f"{i}@{c}")
            for (part, gesture, mods), sigs in sorted(seen.items(), key=lambda kv: str(kv[0])):
                if len(sigs) > 1:
                    desc = "; ".join(f"{s} <- {', '.join(sorted(set(v)))}" for s, v in sigs.items())
                    mod = "+".join(sorted(mods))
                    err(tag, f"{(mod + '+') if mod else ''}{part} ({gesture}) means different port inputs in "
                        f"game-detected contexts, which the layer can't tell apart: {desc}. Detect one context "
                        "in the layer, or mark the target scoped")


def show(lib, m, profile, preset):
    universe = []
    for sid in m["sets"]:
        universe += [i for i in lib.set_universe(sid) if i not in universe]
    universe += [d["id"] for d in m.get("intents", [])]
    labels = {i: (lib.intents.get(i) or next(d for d in m["intents"] if d["id"] == i))["label"] for i in universe}
    for i, o in m.get("overrides", {}).items():
        if "label" in o:
            labels[i] = o["label"]
    by_id, contexts = module_contexts(m, universe, "show")
    layout = resolve_module(lib, m, profile, preset)
    print(f"# {m['title']} ({m['module']}), {profile}{', preset ' + preset if preset else ''}")
    for c, ids in contexts.items():
        print(f"\n## {by_id[c]['label']} [{c}, detected by {by_id[c]['detection']['by']}]")
        over = layout["contexts"].get(c, {})
        ctx_labels = by_id[c].get("labels", {})
        for i in ids:
            entry = over[i] if i in over else layout["bindings"].get(i)
            flat = flatten(entry)
            txt = " / ".join(
                ("+".join(sorted(b[2])) + "+" if b[2] else "") + b[0] + ("" if b[1] == "press" else f" ({b[1]})")
                for b in flat) or "-"
            print(f"  {ctx_labels.get(i, labels[i]):<34} {txt}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--show", help="print a resolved layout: MODULE[:PROFILE[:PRESET]]")
    args = ap.parse_args()

    def load(rel):
        path = os.path.join(args.root, rel)
        with open(path, encoding="utf-8") as f:
            try:
                return json.load(f)
            except json.JSONDecodeError as e:
                err(rel, f"invalid JSON: {e}")
                return None

    schema = load("schema/intents.schema.json")
    profiles = load("profiles.json")
    sets = [(f"sets/{n}", load(f"sets/{n}")) for n in sorted(os.listdir(os.path.join(args.root, "sets")))
            if n.endswith(".json")]
    modules = [(f"modules/{n}", load(f"modules/{n}")) for n in
               sorted(os.listdir(os.path.join(args.root, "modules"))) if n.endswith(".json")]
    docs = [("profiles.json", profiles)] + sets + modules
    if schema is None or any(d is None for _, d in docs):
        return report()
    schema_check(schema, docs)
    if errors:
        return report()  # cross-references assume well-formed documents

    for name, s in sets:
        if name != f"sets/{s['id']}.json":
            err(name, f"file name doesn't match set id {s['id']!r}")
    for name, m in modules:
        if name != f"modules/{m['module']}.json":
            err(name, f"file name doesn't match module id {m['module']!r}")
        if not os.path.isdir(os.path.join(args.root, "..", "..", "..", "modules", m["module"])):
            warn(name, f"no modules/{m['module']} directory in the repo")
    lib = Library(profiles, [s for _, s in sets])
    lib.index()
    for _, s in sets:
        check_set(lib, s)
    for _, m in modules:
        check_module(lib, m)

    if args.show:
        mod, _, rest = args.show.partition(":")
        profile, _, preset = rest.partition(":")
        m = next((m for _, m in modules if m["module"] == mod), None)
        if m is None:
            err("--show", f"no module {mod!r}")
        else:
            show(lib, m, profile or lib.profile_ids[0], preset or None)
            print()
    return report(len(sets), len(modules))


def report(n_sets=0, n_modules=0):
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        print(f"{len(errors)} error(s), {len(warnings)} warning(s)", file=sys.stderr)
        return 1
    print(f"ok: {n_sets} sets, {n_modules} modules, {len(warnings)} warning(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
