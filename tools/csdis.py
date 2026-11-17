#!/usr/bin/env python3
"""disassemble Vice City CLEO .cs scripts (and main.scm chunks).

usage: csdis.py <vc.json> <file.cs> [--summary]
vc.json is Sanny Builder's opcode library (github.com/sannybuilder/library, vc/vc.json).
labels in CLEO scripts are negative offsets from the start of the file, shown as @<hex>.
"""
import json, struct, sys

def load_lib(path):
    d = json.load(open(path))
    ops = {}
    for ext in d['extensions']:
        for c in ext['commands']:
            ops[int(c['id'], 16)] = (c, ext['name'])
    return ops

def params_of(c):
    return (c.get('input') or []) + (c.get('output') or [])

ENDS = (0x0002, 0x004E, 0x0051, 0x05DC, 0x0A93, 0x05F6, 0x0AB2)  # goto, terminate, return, cleo return

def disasm(ops, data, out=None):
    """Follow the control flow from offset 0 (labels are -offset), so data blocks are skipped."""
    used, errors, seen, todo, lines = {}, [], set(), [0], {}
    while todo:
        pc = todo.pop()
        while pc not in seen:
            r = decode(ops, data, pc, used, errors)
            if r is None: break
            start, pc, op, text, labels = r
            seen.add(start); lines[start] = text
            todo += [l for l in labels if l not in seen]
            if op in ENDS: break
    if out is not None:
        out += [lines[k] for k in sorted(lines)]
    return used, errors, max(seen) if seen else 0

def decode(ops, data, pc, used, errors):
        if pc + 2 > len(data):
            errors.append((pc, 'runs off the end')); return None
        start = pc
        raw = struct.unpack_from('<H', data, pc)[0]; pc += 2
        op, neg = raw & 0x7FFF, bool(raw & 0x8000)
        if op not in ops:
            errors.append((start, 'unknown opcode %04X' % op)); return None
        c, ext = ops[op]
        used[op] = used.get(op, 0) + 1
        args, labels = [], []
        plist = params_of(c)
        n = c.get('num_params', len(plist))
        variadic = any(p.get('type') == 'arguments' for p in plist)
        i = 0
        try:
            while True:
                if not variadic and i >= n: break
                t = data[pc]
                ptype = plist[i].get('type') if i < len(plist) else None
                if variadic and i >= n - 1 and t == 0:
                    pc += 1; break
                if t == 0: pc += 1; args.append('<end>'); i += 1; continue
                if t in (1, 4, 5) and ptype == 'label':
                    v = struct.unpack_from({1: '<i', 4: '<b', 5: '<h'}[t], data, pc+1)[0]
                    pc += {1: 5, 4: 2, 5: 3}[t]
                    if v < 0:
                        args.append('@%X' % -v)
                        if op not in (0x05F7, 0x0AC6): labels.append(-v)  # GET_LABEL_POINTER: data, not code
                    else: args.append('main:%X' % v)
                    i += 1; continue
                if t == 1: args.append(str(struct.unpack_from('<i', data, pc+1)[0])); pc += 5
                elif t == 2: args.append('$%d' % (struct.unpack_from('<H', data, pc+1)[0] // 4)); pc += 3
                elif t == 3: args.append('%d@' % struct.unpack_from('<H', data, pc+1)[0]); pc += 3
                elif t == 4: args.append(str(struct.unpack_from('<b', data, pc+1)[0])); pc += 2
                elif t == 5: args.append(str(struct.unpack_from('<h', data, pc+1)[0])); pc += 3
                elif t == 6: args.append('%g' % struct.unpack_from('<f', data, pc+1)[0]); pc += 5
                elif t == 0x0E:  # long string: length byte + chars (CLEO)
                    ln = data[pc+1]; args.append('"%s"' % data[pc+2:pc+2+ln].decode('latin1')); pc += 2 + ln
                else:            # VC inline 8-byte string, no type byte
                    s = data[pc:pc+8].split(b'\0')[0].decode('latin1'); args.append("'%s'" % s); pc += 8
                i += 1
        except (IndexError, struct.error):
            errors.append((start, 'truncated %04X' % op)); return None
        text = '%05X: %s%04X %s %s' % (start, 'NOT ' if neg else '', op, c['name'], ' '.join(args))
        return start, pc, op, text, labels

if __name__ == '__main__':
    ops = load_lib(sys.argv[1])
    data = open(sys.argv[2], 'rb').read()
    out = [] if '--summary' not in sys.argv else None
    used, errors, pc = disasm(ops, data, out)
    if out: print('\n'.join(out))
    for o in sorted(used):
        c, ext = ops[o]
        if ext != 'default' or o > 0x059E:
            print('  CLEO %04X %-34s x%d (%s)' % (o, c['name'], used[o], ext))
    for e in errors: print('  ERROR at %05X: %s' % e)
    print('  last instruction at %X of %X' % (pc, len(data)))
