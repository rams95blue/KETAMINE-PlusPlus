#!/usr/bin/env python3
"""Build check: the Toggle / Value enums in features.h must list the same names, in the same order, as the kToggles /
kValues tables in features.cpp (the menu pairs them by position, so one out of place shifts every label after it)."""
import re, sys, pathlib

src = pathlib.Path(sys.argv[1])
h = (src / "features.h").read_text()
c = (src / "features.cpp").read_text()

def enum(name, end):
    body = h[h.index(f"enum {name} {{") + len(f"enum {name} {{"):h.index(end)]
    body = re.sub(r"//[^\n]*", "", body)
    return [x.strip() for x in body.replace("\n", " ").split(",") if x.strip()]

toggles = enum("Toggle", "ToggleCount")
values = enum("Value", "ValueCount")
tkeys = re.findall(r'\{"(\w+)",\s*"', c[c.index("const ToggleInfo kToggles"):c.index("const ValueInfo kValues")])
vkeys = re.findall(r'\{"(\w+)",', c[c.index("const ValueInfo kValues"):c.index("const char* const kPowerNames")])
alias = {"PauseTime": "FreeCamPauseTime"}  # ini key kept from the original menu
bad = [(i, a, b) for i, (a, b) in enumerate(zip(toggles, tkeys)) if alias.get(a, a) != b]
bad += [(i, a, b) for i, (a, b) in enumerate(zip(values, vkeys)) if a != b]
if len(toggles) != len(tkeys) or len(values) != len(vkeys) or bad:
    for i, a, b in bad:
        print(f"check_tables: entry {i}: enum {a} but table {b}")
    print(f"check_tables: {len(toggles)} toggles / {len(tkeys)} table entries, {len(values)} values / {len(vkeys)}")
    sys.exit(1)
print(f"check_tables: {len(toggles)} toggles and {len(values)} values match")
