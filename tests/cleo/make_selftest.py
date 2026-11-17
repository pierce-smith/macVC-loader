#!/usr/bin/env python3
"""assemble tests/cleo/CLEOSelfTest.cs, a CLEO script that checks the runner.

usage: make_selftest.py [out.cs]
copy the .cs into GameData/CLEO and start or load a game. after ~2 seconds it writes
GameData/CLEO/CLEOSelfTest.ini:  [Result] Sum=42, Text=..., Passed=1 (or Failed=<step>)
steps: 1 CLEO_CALL, 2 label memory, 3 ini files, 4 a Windows pointer variable, 5 calling a
Windows function, 6 a CPlayerInfo field, 7 a ped field.
and shows a help box. F7 then shows another help box (key input).
"""
import struct, sys

class Asm:
    def __init__(s): s.b = bytearray(); s.labels = {}; s.fix = []
    def op(s, code, *args, end=False):
        s.b += struct.pack('<H', code)
        for a in args: s.param(a)
        if end: s.b.append(0)
    def param(s, a):
        if isinstance(a, tuple) and a[0] == 'l':   s.b += bytes([3]) + struct.pack('<H', a[1])         # local var
        elif isinstance(a, tuple) and a[0] == '@': s.b += bytes([1]); s.fix.append((len(s.b), a[1])); s.b += b'\0' * 4
        elif isinstance(a, str):                  e = a.encode('latin1'); s.b += bytes([0x0E, len(e)]) + e
        elif isinstance(a, bytes):                s.b += a.ljust(8, b'\0')[:8]                          # VC 8-char string
        elif isinstance(a, float):                s.b += bytes([6]) + struct.pack('<f', a)
        else:                                     s.b += bytes([1]) + struct.pack('<i', a)
    def label(s, name): s.labels[name] = len(s.b)
    def data(s, raw): s.b += raw
    def build(s):
        for at, name in s.fix: struct.pack_into('<i', s.b, at, -s.labels[name])
        return bytes(s.b)

L = lambda n: ('l', n)
J = lambda n: ('@', n)
INI = r'CLEO\CLEOSelfTest.ini'
a = Asm()
a.op(0x0000)                                   # NOP: a label at offset 0 would mean main.scm 0
a.op(0x03A4, b'CLEOTST')                       # SCRIPT_NAME
a.op(0x0001, 2000)                             # WAIT 2000
a.op(0x0AB1, J('add'), 2, 20, 22, L(0), end=True)  # CLEO_CALL @add 2 20 22 -> 0@
a.op(0x00D6, 0); a.op(0x0039, L(0), 42); a.op(0x004D, J('fail1'))
a.op(0x0AC6, J('data'), L(1))                  # GET_LABEL_POINTER @data -> 1@
a.op(0x0A8D, L(1), 4, 0, L(2))                 # READ_MEMORY 1@ 4 0 -> 2@
a.op(0x00D6, 0); a.op(0x0039, L(2), 0x1234567); a.op(0x004D, J('fail2'))
a.op(0x0AC6, J('buf'), L(3))                   # GET_LABEL_POINTER @buf -> 3@
a.op(0x0AD3, L(3), 'CLEO OK: %d %s', L(0), 'labels', end=True)  # STRING_FORMAT
a.op(0x0AF5, L(3), INI, 'Result', 'Text')      # WRITE_STRING_TO_INI_FILE
a.op(0x0AF1, L(0), INI, 'Result', 'Sum')       # WRITE_INT_TO_INI_FILE
a.op(0x0AF0, INI, 'Result', 'Sum', L(4))       # READ_INT_FROM_INI_FILE -> 4@
a.op(0x00D6, 0); a.op(0x0039, L(4), 42); a.op(0x004D, J('fail3'))
# 4. Windows pointer variable: CWorld::Players[0].m_pPed (0x94AD28) is the player's ped
a.op(0x01F5, 0, L(5))                          # GET_PLAYER_CHAR 0 -> 5@
a.op(0x05E6, L(5), L(6))                       # GET_PED_POINTER 5@ -> 6@
a.op(0x0A8D, 0x94AD28, 4, 0, L(7))             # READ_MEMORY Players[0].m_pPed -> 7@
a.op(0x00D6, 0); a.op(0x003B, L(6), L(7)); a.op(0x004D, J('fail4'))
# 5. Windows function: CPools::GetPedRef (0x4C0020) turns the pointer back into the handle
a.op(0x0AA7, 0x4C0020, 1, 1, L(6), L(8), end=True)
a.op(0x00D6, 0); a.op(0x003B, L(5), L(8)); a.op(0x004D, J('fail5'))
# 6. CPlayerInfo field: Players[0].m_nMoney (0x94ADC8) is the player's money
a.op(0x010B, 0, L(9))                          # STORE_SCORE 0 -> 9@
a.op(0x0A8D, 0x94ADC8, 4, 0, L(10))
a.op(0x00D6, 0); a.op(0x003B, L(9), L(10)); a.op(0x004D, J('fail6'))
# 7. ped field: CPed::m_fHealth (+0x354 on Windows) is above 0
a.op(0x0A8E, L(6), 0x354, L(11))
a.op(0x0A8D, L(11), 4, 0, L(12))
a.op(0x00D6, 0); a.op(0x0021, L(12), 0.0); a.op(0x004D, J('fail7'))
a.op(0x0AF1, 1, INI, 'Result', 'Passed')
a.op(0x0ACA, L(3))                             # PRINT_HELP_STRING 3@
a.label('loop')
a.op(0x0001, 0)
a.op(0x00D6, 0); a.op(0x0AB0, 0x76); a.op(0x004D, J('loop'))   # IS_KEY_PRESSED F7
a.op(0x0ACA, '~g~CLEO runner: F7 pressed')
a.op(0x0001, 500)
a.op(0x0002, J('loop'))
for n in (1, 2, 3, 4, 5, 6, 7):
    a.label('fail%d' % n)
    a.op(0x0AF1, n, INI, 'Result', 'Failed')
    a.op(0x0ACA, '~r~CLEO self-test failed at step %d' % n)
    a.op(0x0A93)
a.label('add')                                 # 0@ + 1@ -> CLEO_RETURN 1 2@
a.op(0x0A8E, L(0), L(1), L(2))
a.op(0x0AB2, 1, L(2), end=True)
a.label('data'); a.data(struct.pack('<i', 0x1234567))
a.label('buf'); a.data(b'\0' * 128)
open(sys.argv[1] if len(sys.argv) > 1 else 'CLEOSelfTest.cs', 'wb').write(a.build())
