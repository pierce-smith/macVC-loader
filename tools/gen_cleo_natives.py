#!/usr/bin/env python3
"""generate loader/cleo_natives.h: every script command JavaScript can call (CLEO Redux style).

usage: gen_cleo_natives.py vc.json > loader/cleo_natives.h
from Sanny Builder's library: name, class and member (for Char.Create / char.setHealth),
argument and result kinds. commands that only make sense in compiled scripts are left out:
jumps and labels, commands that change a variable in place, and variable-length results.
  inputs:  i number, f float, s string (8 characters for game commands), a variable-length list
  outputs: i number, f float, or a class name for handles (wrapped as that class in JS)
"""
import json, sys
d = json.load(open(sys.argv[1]))
classes = {c['name'] for c in d.get('classes', [])}
rows = []
for ext in d['extensions']:
    if ext['name'] == 'imgui': continue
    for c in ext['commands']:
        op = int(c['id'], 16)
        attrs = c.get('attrs') or {}
        if attrs.get('is_unsupported') or attrs.get('is_branch') or attrs.get('is_segment') or attrs.get('is_nop'): continue
        ins, outs, skip = '', [], False
        for p in c.get('input') or []:
            t, src = p.get('type'), p.get('source', '')
            if t == 'label' or src.startswith('var'): skip = True
            elif t == 'arguments': ins += 'a'
            elif t == 'float': ins += 'f'
            elif t in ('string', 'gxt_key', 'zone_key'): ins += 's'
            else: ins += 'i'
        for p in c.get('output') or []:
            t = p.get('type')
            if t in ('string', 'arguments'): skip = True
            elif t == 'float': outs.append('f')
            elif t in classes: outs.append(t)
            else: outs.append('i')
        if skip or 'a' in ins[:-1]: continue
        flags = (1 if attrs.get('is_condition') else 0) | (2 if attrs.get('is_static') else 0) | (4 if attrs.get('is_constructor') else 0)
        names = ','.join(p.get('name') or 'result' for p in c.get('output') or [])
        rows.append((op, c['name'], c.get('class', ''), c.get('member', ''), flags, ins, ','.join(outs), names))
seen, out = set(), []
for r in rows:
    if r[0] in seen: continue
    seen.add(r[0]); out.append(r)
print('// every script command JS can call, CLEO Redux style (from Sanny Builder\'s library, vc.json).')
print('// { opcode, name, class, member, flags (1 condition, 2 static, 4 constructor), inputs, outputs, output names }')
for r in out:
    print('{ 0x%04X, "%s", "%s", "%s", %d, "%s", "%s", "%s" },' % r)
print('// %d commands' % len(out), file=sys.stderr)
