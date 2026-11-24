#!/usr/bin/env python3
"""Disassemble Gekko code from a .dol: dasm.py main.dol START END"""
import sys, struct
from capstone import *
dol = open(sys.argv[1], 'rb').read()
off = struct.unpack('>18I', dol[0:72]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
def read(a, n):
    for i in range(18):
        if size[i] and addr[i] <= a < addr[i] + size[i]:
            o = off[i] + a - addr[i]; return dol[o:o + n]
    return None
s, e = int(sys.argv[2], 16), int(sys.argv[3], 16)
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN | CS_MODE_PS)
code = read(s, e - s)
for i in md.disasm(code, s): print(f"{i.address:08X}: {i.bytes.hex()} {i.mnemonic} {i.op_str}")
