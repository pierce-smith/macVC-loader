#!/usr/bin/env python3
# writes loader/gxt_vanilla.h: 32-bit fingerprints (FNV-1a) of every key and
# string in the original Vice City 1.0 .gxt files. Mod Loader uses them to tell
# which strings a mod's .gxt actually changes, so only those are applied on top
# of macVC's own text. only hashes are stored, never the game's text.
#   tools/gxt_hashes.py /path/to/GTA_VC_1.0/TEXT > loader/gxt_vanilla.h
import struct, sys, os

def fnv(b):
    h = 0x811c9dc5
    for c in b:
        h ^= c
        h = (h * 0x01000193) & 0xffffffff
    return h

def entries(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'TABL'
    n = struct.unpack('<I', d[4:8])[0]
    for k in range(n // 12):
        name = d[8 + k*12: 16 + k*12].split(b'\0')[0]
        off = struct.unpack('<I', d[16 + k*12: 20 + k*12])[0]
        o = off if name == b'MAIN' else off + 8
        kn = struct.unpack('<I', d[o+4:o+8])[0]; ks = o + 8; td = ks + kn; ds = td + 8
        for j in range(kn // 12):
            eo = struct.unpack('<I', d[ks + j*12: ks + j*12 + 4])[0]
            key = d[ks + j*12 + 4: ks + j*12 + 12].split(b'\0')[0]
            e = ds + eo
            while d[e:e+2] != b'\0\0': e += 2
            yield name, key, d[ds + eo: e]

out = ['// fingerprints of the original Vice City 1.0 text strings, from tools/gxt_hashes.py.',
       '// FNV-1a of "TABLE:KEY" and of each string (UTF-16LE bytes). no game text is stored.',
       '#pragma once', '#include <stdint.h>',
       'typedef struct { uint32_t key, value; } GxtHash;']
langs = []
for lang in ('american', 'french', 'german', 'italian', 'spanish'):
    p = next((os.path.join(sys.argv[1], f) for f in os.listdir(sys.argv[1]) if f.lower() == lang + '.gxt'), None)
    if not p: continue
    rows = sorted((fnv(t + b':' + k), fnv(v)) for t, k, v in entries(p))
    out.append('static const GxtHash GXT_%s[] = {' % lang.upper())
    for i in range(0, len(rows), 6):
        out.append('    ' + ' '.join('{0x%08x, 0x%08x},' % r for r in rows[i:i+6]))
    out.append('};')
    langs.append(lang)
out.append('static const struct { const char *file; const GxtHash *tab; int n; } GXT_VANILLA[] = {')
for l in langs: out.append('    {"%s.gxt", GXT_%s, (int)(sizeof GXT_%s / sizeof *GXT_%s)},' % (l, l.upper(), l.upper(), l.upper()))
out.append('};')
print('\n'.join(out))
