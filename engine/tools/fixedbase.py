#!/usr/bin/env python3
"""fixedbase.py webkit.dll [--undo] — clears (or sets again) the DYNAMIC_BASE bit of a PE file, in place.

lld-link refuses /dynamicbase:no on ARM, and the phone relocates webkit.dll (loaded at 0x64670000 instead of its
0x10000000): 97% of the code pages carry MOV32 relocations, and a relocated page is the process's own memory instead of
a page of the file that can be dropped and read again (PLAN.md §56.4 point 2, §74). Without the bit the loader should
map the file at its preferred base when that is free, and relocate only when it is not (.reloc stays in the file).
A test: the phone's loader may refuse such a file. tools/browser uses it with ENGINE_FIXED_BASE=1.
"""
import struct, sys

path = sys.argv[1]
undo = "--undo" in sys.argv
with open(path, "r+b") as f:
    data = f.read(4096)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x10B, "not PE32"
    at = optional + 70  # DllCharacteristics
    flags = struct.unpack_from("<H", data, at)[0]
    new = flags | 0x40 if undo else flags & ~0x40
    f.seek(at)
    f.write(struct.pack("<H", new))
    base = struct.unpack_from("<I", data, optional + 28)[0]
    print("%s: DllCharacteristics %#06x -> %#06x, base %#x" % (path, flags, new, base))
