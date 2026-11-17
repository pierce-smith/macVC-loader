#!/usr/bin/env python3
"""generate loader/cleo_addrs.h: Windows gta-vc.exe 1.0 data addresses -> macVC symbols.

usage: gen_cleo_addrs.py <plugin-sdk/plugin_vc/game_vc> <macVC binary> > loader/cleo_addrs.h

plugin-sdk (github.com/DK22Pac/plugin-sdk) names the 1.0 address of each game variable;
macVC exports the same variables by name. variables of plain numbers (or arrays of them)
have the same layout on both. pointer variables are marked, because macVC's pointers
are 8 bytes; CWorld::Players is marked as a CPlayerInfo array. other structs are left out.
the table holds only addresses and symbol names, resolved by name at runtime.
"""
import difflib, os, re, subprocess, sys

PTR_KIND = {'CRunningScript': 'S', 'CPed': 'P', 'CPlayerPed': 'P', 'CVehicle': 'V', 'CAutomobile': 'V', 'CHeli': 'V', 'CBoat': 'V', 'CObject': 'O'}
PRIM = {'char': 1, 'bool': 1, 'unsigned char': 1, 'signed char': 1, 'uchar': 1, 'int8_t': 1, 'uint8_t': 1,
        'short': 2, 'unsigned short': 2, 'ushort': 2, 'wchar': 2, 'int16_t': 2, 'uint16_t': 2,
        'int': 4, 'unsigned int': 4, 'uint': 4, 'float': 4, 'int32_t': 4, 'uint32_t': 4, 'unsigned': 4,
        'long': 4, 'unsigned long': 4, 'eLevelName': 4, 'eWeatherType': 4}

# type &Class::name = *(Type*)0xADDR;   type *Class::name = (Type*)0xADDR;   ... GLOBAL_ADDRESS_BY_VERSION(0x1, ..)
VAR = re.compile(r'^\s*(?:PLUGIN_VARIABLE\s+)?([A-Za-z_][\w ]*?)\s*(\(?\s*[&*]+)\s*([\w:]+)\s*\)?\s*(\[\s*\w*\s*\])*\s*=\s*(.*?);')
ADDR = re.compile(r'(?:GLOBAL_ADDRESS_BY_VERSION\(\s*)?0x([0-9A-Fa-f]{6,8})')
ARR = re.compile(r'\[\s*(\d+)\s*\]')

def main():
    sdk, binary = sys.argv[1], sys.argv[2]
    nm = subprocess.run(['nm', binary], capture_output=True, text=True).stdout.split('\n')
    dem = subprocess.run(['c++filt'], input='\n'.join(l.split(' ')[-1] for l in nm), capture_output=True, text=True).stdout.split('\n')
    by_name = {}
    for line, d in zip(nm, dem):
        p = line.split(' ')
        if len(p) < 3 or p[1] not in 'SDBsdb': continue
        by_name.setdefault(d.lstrip('_') if not d.startswith('__Z') else d, p[2][1:] if p[2].startswith('_') else p[2])
    rows3 = [l.split(' ') for l in nm]
    addrs = sorted((int(p[0], 16), p[2][1:]) for p in rows3 if len(p) >= 3 and p[0] and p[1] in 'SDBsdb')
    sizes = {addrs[i][1]: addrs[i + 1][0] - addrs[i][0] for i in range(len(addrs) - 1)}
    rows, seen = [], set()
    for f in sorted(os.listdir(sdk)):
        if not f.endswith('.cpp'): continue
        for line in open(os.path.join(sdk, f), encoding='latin1'):
            m = VAR.match(line)
            if not m: continue
            typ, deref, name, rhs = m.group(1).strip(), m.group(2).replace(' ', '').replace('(', ''), m.group(3), m.group(5)
            typ = re.sub(r'\b(const|volatile|static|struct|enum|class)\b', '', typ).strip()
            a = ADDR.search(rhs)
            if not a: continue
            win = int(a.group(1), 16)
            if win < 0x600000 or win in seen: continue
            mac = by_name.get(name)
            if not mac: # spelling differs (CCamera::m_fMouseAccelHorzntal vs ...Horzntl): closest name in the class
                cls = name.rsplit('::', 1)[0] + '::' if '::' in name else ''
                near = difflib.get_close_matches(name, [k for k in by_name if k.startswith(cls)], 1, 0.9)
                mac = by_name[near[0]] if near else None
            if not mac: continue
            n = 1
            for side in (line.split('=')[0], rhs):  # "char(&X)[30] = *(char(*)[30])..." names it twice
                k = 1
                for d in ARR.findall(side): k *= int(d)
                n = max(n, k)
            ptr = '0'
            if deref in ('*&', '**'):  # a pointer variable, or (with **) an array of pointers
                ptr = PTR_KIND.get(typ, 'p')
                count = max(1, sizes.get(mac, 8) // 8) if deref == '**' else 1
                size = 4 * count     # 4-byte pointers on Windows; macVC's are 8
            elif typ == 'CPlayerInfo' and deref == '*':
                ptr, size = 'I', 0x170 * max(1, sizes.get(mac, 0x1A0) // 0x1A0) # struct array, fields in cleo_fields.h
            elif typ in PRIM:
                size = PRIM[typ] * n
                if size == 1 and '*' in deref and mac in sizes: size = min(sizes[mac], 4096) # char *X = (char*)0x..
                if mac in sizes: size = min(size, sizes[mac]) # never past the end of macVC's variable
            else: continue
            seen.add(win)
            rows.append((win, size, mac, name, ptr))
    rows.sort()
    print('// Windows gta-vc.exe 1.0 game variables -> macVC symbols (from plugin-sdk and the macVC symbol table).')
    print("// { Windows 1.0 address, size in bytes (Windows), macVC symbol (mangled, no leading _), name, kind }")
    print("// kind: '0' plain numbers; P/V/O/S/p pointers (to ped/vehicle/object/script/other), 8 bytes each in macVC;")
    print("//       'I' an array of CPlayerInfo (0x170 bytes each on Windows, fields in cleo_fields.h)")
    for win, size, mac, name, ptr in rows:
        print("{ 0x%06X, %d, \"%s\", \"%s\", '%s' }," % (win, size, mac, name, ptr))
    print('// %d variables' % len(rows), file=sys.stderr)

main()
