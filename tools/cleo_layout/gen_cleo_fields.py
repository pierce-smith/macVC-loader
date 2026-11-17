#!/usr/bin/env python3
"""generate loader/cleo_fields.h: Windows struct field offsets -> macVC offsets.

usage: gen_cleo_fields.py <plugin-sdk checkout> > ../../loader/cleo_fields.h

macVC is built from reVC, which keeps the original field order and types. so compiling
plugin-sdk's VC headers (whose VALIDATE_OFFSET lines give every Windows offset) for
64-bit ARM gives macVC's offsets. two corrections make it match this build's code:
  - CAutoPilot's path nodes are pointers in reVC (plugin-sdk declares 4-byte ints)
only fields inside RANGES are emitted: spans whose two ends were checked against
offsets in macVC's own code (see the comments). everything else stays unmapped.
pointer fields are marked so the runner hands scripts a 32-bit stand-in.
"""
import glob, os, re, shutil, subprocess, sys, tempfile

sdk = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))

# (kind, class, first Windows offset, end Windows offset): checked anchors in brackets
RANGES = [
    # CPhysical: move speed 0x70->0x84 and turn speed 0x7C->0x90 (CTheScripts::Process),
    # mass 0xB8->0xCC (CVehicle::AddPassenger), ends where CVehicle's handling 0x120->0x160 starts
    ('E', 'CPhysical', 0x64, 0x120),
    # CPed: health 0x354->0x44C, armour, vehicle 0x3A8->0x4B8 (FindPlayerVehicle),
    # ends where CPlayerPed's wanted 0x5F4->0x778 starts (CPlayerPed::SetWantedLevel)
    ('P', 'CPed', 0x120, 0x5F4),
    ('P', 'CPlayerPed', 0x5F4, 0x640),   # wanted, drunkenness 0x638->0x7CC (ProcessControl)
    # CVehicle: handling 0x120->0x160 (CAutomobile::Fix), driver 0x1A8->0x218 (SetDriver),
    # passengers 0x1AC->0x220 and max 0x1D0->0x264 (AddPassenger), health 0x204->0x2A0 (BlowUpCar)
    ('V', 'CVehicle', 0x120, 0x208),
    ('O', 'CObject', 0x120, 0x170),      # object type 0x16C->0x1B4 (CObject::CanBeDeleted)
    # CPlayerInfo (CWorld::Players): money 0xA0->0xC8 (sdk.c), state 0xCC->0xF8, death fade 0xD0->0xFC
    # and busted audio 0x148->0x174 (CPlayerInfo::ArrestPlayer); the skin texture pointer ends at 0x1A0
    ('I', 'CPlayerInfo', 0x0, 0x170),
]
KIND_OF_PTR = {'CPed': 'P', 'CPlayerPed': 'P', 'CCopPed': 'P', 'CCivilianPed': 'P', 'CVehicle': 'V',
               'CAutomobile': 'V', 'CBike': 'V', 'CObject': 'O'}

tmp = tempfile.mkdtemp()
gv, sh = os.path.join(tmp, 'gv'), os.path.join(tmp, 'sh')
shutil.copytree(os.path.join(sdk, 'plugin_vc', 'game_vc'), gv)
shutil.copytree(os.path.join(sdk, 'shared'), sh)
shutil.copy(os.path.join(here, 'PluginBase.h'), os.path.join(sh, 'PluginBase.h'))
inc = os.path.join(tmp, 'inc'); os.mkdir(inc)
for h in ('windows.h', 'dinput.h'): shutil.copy(os.path.join(here, h), inc)
for f in glob.glob(os.path.join(gv, 'meta', '*.h')): open(f, 'w').write('#pragma once\n')
for f in glob.glob(os.path.join(gv, '*.h')):
    t = open(f, encoding='latin1').read()
    t2 = t.replace('operator new(unsigned int', 'operator new(size_t').replace('operator new[](unsigned int', 'operator new[](size_t')
    t2 = t2.replace('CNodeAddress m_aPathFindNodesInfo[8];', 'void *m_aPathFindNodesInfo[8]; // reVC: CPathNode *[8]')
    if t2 != t: open(f, 'w', encoding='latin1').write(t2)

rows, heads = [], set()
for f in sorted(glob.glob(os.path.join(gv, '*.h'))):
    for m in re.finditer(r'VALIDATE_OFFSET\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)', open(f, encoding='latin1').read()):
        if any(m.group(1) == r[1] for r in RANGES):
            rows.append((m.group(1), m.group(2), int(m.group(3), 0))); heads.add(os.path.basename(f))
src = ['#include <cstdio>'] + ['#include "%s"' % h for h in sorted(heads)]
src += ['template <class T> const char *tn() { return __PRETTY_FUNCTION__; }', 'int main() {']
for c, mem, o in rows:
    src.append('  printf("%s %s 0x%x 0x%%zx %%zu|%%s\\n", (size_t)__builtin_offsetof(%s, %s), sizeof(((%s *)0)->%s), tn<decltype(((%s *)0)->%s)>());'
               % (c, mem, o, c, mem, c, mem, c, mem))
src.append('  return 0;\n}')
open(os.path.join(tmp, 'lay.cpp'), 'w').write('\n'.join(src))
cc = ['xcrun', 'clang++', '-target', 'arm64-apple-macos15', '-std=c++20', '-w', '-fms-extensions', '-DGTAVC', '-DRW',
      '-I' + inc, '-I' + gv, '-I' + os.path.join(gv, 'rw'), '-I' + os.path.join(gv, 'enums'),
      '-idirafter', sh, '-idirafter', os.path.join(sh, 'game'), os.path.join(tmp, 'lay.cpp'), '-o', os.path.join(tmp, 'lay')]
r = subprocess.run(cc, capture_output=True, text=True)
if r.returncode: sys.exit(r.stderr[-3000:])
out = subprocess.run([os.path.join(tmp, 'lay')], capture_output=True, text=True).stdout.split('\n')
shutil.rmtree(tmp)

fields = []
for line in out:
    if not line: continue
    head, typ = line.split('|', 1)
    typ = typ[typ.index('T = ') + 4:-1] if 'T = ' in typ else typ
    c, mem, win, mac, size = head.split()
    win, mac, size = int(win, 16), int(mac, 16), int(size)
    fields.append([c, mem, win, mac, size, typ])
fields.sort(key=lambda f: (f[0], f[2]))
print('// Windows struct field offsets -> macVC offsets for peds, vehicles, objects and player info (from plugin-sdk, checked against macVC\'s code).')
print('// { kind, Windows offset, Windows size, macVC offset, element: 0 plain / P,V,O,p pointer to that, name }')
n = 0
for i, (c, mem, win, mac, size, typ) in enumerate(fields):
    rng = [r for r in RANGES if r[1] == c and r[2] <= win < r[3]]
    if not rng: continue
    nxt = [f for f in fields[i + 1:] if f[0] == c]
    win_end = min(nxt[0][2], rng[0][3]) if nxt else rng[0][3]
    count = 1
    m = re.search(r'\[(\d+)\]', typ)
    if m: count = int(m.group(1))
    if '*' in typ and '(*' not in typ:  # pointer, or array of pointers
        if size != 8 * count: continue
        base = typ.split('*')[0].replace('const', '').replace('class', '').strip()
        ptr = KIND_OF_PTR.get(base, 'p')
        win_size = 4 * count
    else:
        if '*' in typ: continue
        ptr, win_size = '0', size
        if win + size > win_end: continue  # the Windows field is smaller: contains pointers
    print("{ '%s', 0x%X, %d, 0x%X, '%s', \"%s::%s\" }," % (rng[0][0], win, win_size, mac, ptr, c, mem))
    n += 1
print('// %d fields' % n, file=sys.stderr)
