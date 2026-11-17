#!/usr/bin/env python3
"""will this CLEO script work on macVC? checks .cs files against the CLEO runner.

usage: cleo_compat.py <vc.json> script.cs [more.cs ...]   (prints a Markdown table)
uses the runner's own tables (loader/cleo_addrs.h, cleo_funcs.h and the opcodes cleo.c
handles), so it stays in step with the loader. it reads the script statically: memory
addresses computed at run time can't be seen, and the log is the final word.
  works         everything it uses is supported
  partly        some opcodes, game memory or functions are skipped (each logged in game)
  needs a port  it patches gta-vc.exe's x86 code, which can't run on ARM
  won't run     it needs opcodes from a Windows .cleo plugin
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csdis

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'loader')
src = open(os.path.join(root, 'cleo.c')).read()
body = src[src.index('static int8_t run_cleo_op('):src.index('static int is_cleo_op(')]
handled = {int(x, 16) for x in re.findall(r'case 0x([0-9A-Fa-f]{4}):', body)}
handled |= {0x0AF0, 0x0AF1, 0x0AF2, 0x0AF3, 0x0AF4, 0x0AF5}
var_rows = [l for l in open(os.path.join(root, 'cleo_addrs.h')) if l.startswith('{')]
VARS = [(int(r.split(',')[0][2:], 16), int(r.split(',')[1])) for r in var_rows]  # (pointer and struct fields: checked in game)
FUNCS = {int(l.split(',')[0][2:], 16) for l in open(os.path.join(root, 'cleo_funcs.h')) if l.startswith('{')}
MEM = {0x05DF, 0x05E0, 0x0A8C, 0x0A8D}
CALL = {0x05E1, 0x05E2, 0x05E3, 0x05E4, 0x0AA5, 0x0AA6, 0x0AA7, 0x0AA8}

def var_ok(a): return any(w <= a < w + s for w, s in VARS)

ops = csdis.load_lib(sys.argv[1])
def pl(n, one, many=None): return one if n == 1 else (many or one + 's')

print('| script | status | notes |\n|---|---|---|')
for path in sys.argv[2:]:
    data = open(path, 'rb').read()
    lines = []
    used, errors, _ = csdis.disasm(ops, data, lines)
    notes, code, data_bad, data_ok, fn_bad, fn_ok, skipped = [], set(), set(), set(), set(), set(), set()
    computed = set()  # Windows addresses used as a base for arithmetic (an array index, a struct walk)
    for l in lines:
        m = re.match(r'[0-9A-F]+: (?:NOT )?([0-9A-F]{4}) \S+ ?(\S*)', l)
        if not m: continue
        op, a1 = int(m.group(1), 16), m.group(2)
        if op >= 0x05DC and op not in handled: skipped.add('%04X %s' % (op, ops[op][0]['name']))
        if op in (0x0004, 0x0006, 0x0008, 0x000A, 0x0A8E):  # SET/ADD with a literal in gta-vc.exe's data
            for v in re.findall(r' (-?\d+)(?= |$)', l):
                a = int(v) & 0xFFFFFFFF
                if 0x67D000 <= a < 0xC00000 and not var_ok(a): computed.add(a)
        if op in MEM | CALL and re.fullmatch(r'-?\d+', a1):
            a = int(a1) & 0xFFFFFFFF
            if a < 0x400000: continue
            if op in CALL: (fn_ok if a in FUNCS else fn_bad).add(a)
            elif a < 0x67D000: code.add(a)
            else: (data_ok if var_ok(a) else data_bad).add(a)
    unknown = [e[1] for e in errors if 'unknown opcode' in e[1]]
    if unknown: status = "won't run"; notes.append('needs a Windows .cleo plugin (%s)' % ', '.join(sorted(set(u.split()[-1] for u in unknown))))
    elif code: status = 'needs a port'; notes.append('patches %d x86 code %s' % (len(code), pl(len(code), 'address', 'addresses')))
    elif data_bad or fn_bad or skipped or computed: status = 'partly'
    else: status = 'works'
    if data_ok: notes.append('%d game %s mapped' % (len(data_ok), pl(len(data_ok), 'variable')))
    if data_bad: notes.append('%d unmapped game %s: %s' % (len(data_bad), pl(len(data_bad), 'address', 'addresses'), ', '.join('0x%X' % a for a in sorted(data_bad)[:4])))
    if computed: notes.append('computes addresses from unmapped 0x%s (check the log)' % ', 0x'.join('%X' % a for a in sorted(computed)[:3]))
    if fn_ok: notes.append('%d game %s mapped' % (len(fn_ok), pl(len(fn_ok), 'function')))
    if fn_bad: notes.append('%d unknown %s' % (len(fn_bad), pl(len(fn_bad), 'function')))
    if skipped: notes.append('skipped: ' + ', '.join(sorted(skipped)))
    print('| %s | %s | %s |' % (os.path.basename(path), status, '; '.join(notes) or '-'))
