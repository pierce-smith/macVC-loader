#!/usr/bin/env python3
"""generate loader/cleo_ops.h from Sanny Builder's VC opcode library (vc/vc.json).

usage: gen_cleo_ops.py vc.json > loader/cleo_ops.h
for every CLEO-extension opcode: its name, parameter count, whether the last parameter is
a variable-length list ended by a 0 byte, and whether it's a condition. the runner uses this
to step over an opcode it doesn't implement yet, so the script keeps going.
"""
import json, sys
d = json.load(open(sys.argv[1]))
rows = {}
for ext in d['extensions']:
    if ext['name'] in ('default', 'imgui'): continue
    for c in ext['commands']:
        op = int(c['id'], 16)
        if op < 0x05DC or op > 0x7FFF: continue
        params = (c.get('input') or []) + (c.get('output') or [])
        variadic = any(p.get('type') == 'arguments' for p in params)
        cond = bool((c.get('attrs') or {}).get('is_condition'))
        rows.setdefault(op, (c.get('num_params', len(params)), variadic, cond, c['name']))
print('// CLEO opcodes the runner knows by name (from Sanny Builder\'s library, vc.json).')
print('// { opcode, parameters, variadic, condition, name }')
for op in sorted(rows):
    n, v, c, name = rows[op]
    print('{ 0x%04X, %d, %d, %d, "%s" },' % (op, n, int(v), int(c), name))
