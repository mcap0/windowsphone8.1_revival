#!/usr/bin/env python3
"""peimports.py <pe> [-v]: imported DLLs (and with -v the functions) of a PE file, plus its machine type."""
import struct
import sys


def imports(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    machine = struct.unpack_from("<H", d, e + 4)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    opt = e + 24
    dirs = opt + (96 if struct.unpack_from("<H", d, opt)[0] == 0x10B else 112)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        vsz, va, rsz, rp = struct.unpack_from("<IIII", d, so + 8)
        secs.append((va, max(vsz, rsz), rp))
        so += 40

    def off(rva):
        return next(rp + rva - va for va, size, rp in secs if va <= rva < va + size)

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b"\0", o)].decode("ascii")

    result = []
    imp_rva = struct.unpack_from("<I", d, dirs + 8)[0]
    if imp_rva:
        q = off(imp_rva)
        while True:
            ilt, ts, fc, nm, iat = struct.unpack_from("<IIIII", d, q)
            if not nm:
                break
            fns = []
            t = off(ilt or iat)
            while True:
                v = struct.unpack_from("<I", d, t)[0]
                if not v:
                    break
                fns.append("#%d" % (v & 0xFFFF) if v & 0x80000000 else cstr(v + 2))
                t += 4
            result.append((cstr(nm), fns))
            q += 20
    return machine, result


if __name__ == "__main__":
    machine, result = imports(sys.argv[1])
    print("machine %#x%s" % (machine, " (ARMNT)" if machine == 0x1C4 else ""))
    for dll, fns in result:
        print("%s %d" % (dll, len(fns)))
        if "-v" in sys.argv:
            print("   " + " ".join(sorted(fns)))
