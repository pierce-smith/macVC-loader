#!/usr/bin/env python3
"""generate loader/cleo_funcs.h: Windows gta-vc.exe 1.0 function addresses -> macVC functions.

usage: gen_cleo_funcs.py <plugin-sdk/plugin_vc/game_vc> <macVC binary> > loader/cleo_funcs.h

plugin-sdk gives each function's 1.0 address, name and return type ("// Converted from
thiscall int CPed::Foo(int) 0x4F..." comments, meta/*.h, and Call<0x...> bodies). the
function is looked up in macVC by name and argument count; macVC's own mangled name gives
the argument types, which decide the ARM64 registers. functions whose arguments can't be
passed from a script (structs by value, doubles, more than 8 of a kind) are left out.
columns: { address, method (thiscall), macVC symbol, return kind, argument kinds }
  return: v void, i int, f float, P CPed*, V CVehicle*, O CObject*, p other pointer
  args:   i int-like, f float, p pointer or reference, 3 CVector by value (3 floats)
"""
import os, re, subprocess, sys

sdk, binary = sys.argv[1], sys.argv[2]

def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<(': depth += 1
        elif ch in '>)': depth -= 1
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip() and cur.strip() != 'void': out.append(cur.strip())
    return out

# plugin-sdk: address -> (name, ret, nargs, method)
funcs = {}
CONV = re.compile(r'^//\s*Converted from\s+(cdecl|thiscall|stdcall|fastcall)?\s*(.+?)\s+([\w:~]+::[\w~]+|\w+)\s*\((.*)\)\s*(?:const)?\s*0x([0-9A-Fa-f]+)')
for f in sorted(os.listdir(sdk)):
    if not f.endswith('.cpp'): continue
    lines = open(os.path.join(sdk, f), encoding='latin1').read().split('\n')
    for i, line in enumerate(lines):
        m = CONV.match(line.strip())
        if m:
            conv, ret, name, args, addr = m.groups()
            a = int(addr, 16)
            if a >= 0x401000: funcs.setdefault(a, (name, ret.strip(), len(split_args(args)), conv == 'thiscall'))
            continue
        d = re.match(r'^([\w:<>\*& ]+?)\s+([\w]+::[\w~]+)\s*\((.*)\)\s*(const)?\s*\{?\s*$', line)
        if d and not line.startswith((' ', '\t')):
            body = ' '.join(lines[i + 1:i + 6])
            c = re.search(r'Call(Method)?(AndReturn)?<[^>]*?0x([0-9A-Fa-f]{6})', body)
            if c:
                a = int(c.group(3), 16)
                funcs.setdefault(a, (d.group(2), d.group(1).replace('static', '').strip(), len(split_args(d.group(3))), bool(c.group(1))))
for f in sorted(os.listdir(os.path.join(sdk, 'meta'))):
    txt = open(os.path.join(sdk, 'meta', f), encoding='latin1').read()
    for blk in re.findall(r'META_BEGIN(?:_OVERLOADED)?\((.*?)\)(.*?)META_END', txt, re.S):
        name, body = blk[0].split(',')[0].strip(), blk[1]
        ma = re.search(r'MvAddresses<\s*0x([0-9A-Fa-f]+)', body)
        md = re.search(r'def_t\s*=\s*(.+?)\((.*?)\);', body)
        meth = 'Thiscall' in body
        if not ma or not md: continue
        args = split_args(md.group(2))
        if meth and args: args = args[1:]
        funcs.setdefault(int(ma.group(1), 16), (name, md.group(1).strip(), len(args), meth))

# plugin-sdk names that the real gta-vc.exe 1.0 contradicts (checked by disassembling it)
FIXES = {
    0x5259F0: ('CPed::RemoveBodyPart', 'void', 2, True), # tests CGame::nastyGame, stores the part at +0x56A
}
for a, f in FIXES.items(): funcs[a] = f

# macVC: Class::Name -> [(mangled, [arg types])]
nm = [l.split(' ') for l in subprocess.run(['nm', binary], capture_output=True, text=True).stdout.split('\n')]
syms = [p[2] for p in nm if len(p) >= 3 and p[1] in 'Tt' and p[2].startswith('__Z')]
dem = subprocess.run(['c++filt'], input='\n'.join(s[1:] for s in syms), capture_output=True, text=True).stdout.split('\n')
mac = {}
for s, d in zip(syms, dem):
    m = re.match(r'^(.*?)\((.*)\)( const)?$', d)
    if not m or '.cold' in d: continue
    mac.setdefault(m.group(1), []).append((s[1:], split_args(m.group(2))))

def kind(t):
    t = t.replace('const', '').strip()
    if t.endswith('*') or t.endswith('&'): return 'p'
    if t in ('float',): return 'f'
    if t == 'CVector': return '3'
    if t in ('int', 'unsigned int', 'short', 'unsigned short', 'char', 'unsigned char', 'signed char', 'bool',
             'long', 'unsigned long', 'PedNode') or t.startswith('e') or t.startswith('E'): return 'i'
    return None

def ret_kind(t):
    t = t.replace('const', '').replace('static', '').replace('virtual', '').strip()
    if t == 'void': return 'v'
    if t == 'float': return 'f'
    if t.endswith('*'):
        return {'CPed': 'P', 'CPlayerPed': 'P', 'CCivilianPed': 'P', 'CCopPed': 'P', 'CVehicle': 'V',
                'CAutomobile': 'V', 'CBike': 'V', 'CBoat': 'V', 'CHeli': 'V', 'CObject': 'O'}.get(t[:-1].strip(), 'p')
    if t in ('double', 'CVector', 'CRGBA', 'CRect'): return None
    return 'i'

rows, skipped = [], 0
for a in sorted(funcs):
    if not 0x401000 <= a < 0x67D000: continue  # gta-vc.exe 1.0 code
    name, ret, n, meth = funcs[a]
    cands = [c for c in mac.get(name, []) if len(c[1]) == n]
    rk = ret_kind(ret)
    if len(cands) != 1 or rk is None: skipped += 1; continue
    sym, types = cands[0]
    ks = [kind(t) for t in types]
    if None in ks: skipped += 1; continue
    sig = ''.join(ks)
    if sum(1 for k in sig if k in 'ip') + meth > 8 or sum(3 if k == '3' else 1 for k in sig if k in 'f3') > 8: skipped += 1; continue
    rows.append((a, int(meth), sym, rk, sig, name))
print('// Windows gta-vc.exe 1.0 game functions -> macVC symbols and their ARM64 arguments (from plugin-sdk and the macVC symbol table).')
print('// { Windows 1.0 address, thiscall, macVC symbol, return kind, argument kinds, name }')
for r in rows: print('{ 0x%06X, %d, "%s", \'%s\', "%s", "%s" },' % r)
print('// %d functions, %d left out' % (len(rows), skipped), file=sys.stderr)
