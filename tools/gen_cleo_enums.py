#!/usr/bin/env python3
"""generate loader/cleo_enums.h: CLEO Redux's enums for JavaScript (WeaponType.Pistol...).

usage: gen_cleo_enums.py enums.json > loader/cleo_enums.h
enums.json is Sanny Builder's library vc/enums.json. a member without a value
continues from the previous one (as in TypeScript); ImGui enums are left out
because macVC has no ImGui.
"""
import json, sys
d = json.load(open(sys.argv[1]))
out = {}
for name, members in d.items():
    if name.startswith('ImGui'):
        continue
    e, nxt = {}, 0
    for k, v in members.items():
        if v is None:
            v = nxt
        e[k] = v
        if isinstance(v, int):
            nxt = v + 1
    out[name] = e
js = json.dumps(out, separators=(',', ':'))
print('// CLEO Redux enums for JS scripts (from Sanny Builder\'s library, vc/enums.json).')
print('// %d enums as JSON; the JS prelude turns each one into a frozen global object.' % len(out))
print('static const char CLEO_ENUMS_JSON[] =')
for i in range(0, len(js), 100):
    print('    "%s"' % js[i:i + 100].replace('\\', '\\\\').replace('"', '\\"'))
print('    ;')
