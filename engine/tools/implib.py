#!/usr/bin/env python3
"""implib.py <dll>... -o <outdir>

Import libraries made from the phone's own system DLLs (ref/mainos/fs/Windows/system32): every export of the DLL
becomes an import in <outdir>/<name>.lib (via llvm-dlltool, ARMNT). A function the phone doesn't have then fails at
link time on the PC instead of at load time on the phone. WP8.1 has no real api-ms-win-* files (APISETSCHEMA maps
them onto KERNELBASE.DLL), so the engine imports straight from KERNELBASE/WS2_32/DWRITE."""
import argparse
import os
import struct
import subprocess
import sys


def exports(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e:e + 4] != b"PE\0\0":
        raise SystemExit("%s: not a PE file" % path)
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    opt = e + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dirs = opt + (96 if magic == 0x10B else 112)
    exp_rva, exp_size = struct.unpack_from("<II", d, dirs)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        vsz, va, rsz, rp = struct.unpack_from("<IIII", d, so + 8)
        secs.append((va, max(vsz, rsz), rp))
        so += 40

    def off(rva):
        for va, size, rp in secs:
            if va <= rva < va + size:
                return rp + rva - va
        raise ValueError("rva %#x outside sections" % rva)

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b"\0", o)].decode("ascii")

    if not exp_rva:
        return []
    q = off(exp_rva)
    nfun, nnames, afun, anames, aords = struct.unpack_from("<IIIII", d, q + 20)
    names = []
    for i in range(nnames):
        names.append(cstr(struct.unpack_from("<I", d, off(anames) + 4 * i)[0]))
    return sorted(set(names))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dlls", nargs="+")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("--dlltool", default="llvm-dlltool")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    for dll in args.dlls:
        base = os.path.basename(dll)
        stem = os.path.splitext(base)[0].lower()
        # KERNELBASE and NTDLL also export C runtime functions (_initterm, atexit, memcpy...): leave those to the
        # static CRT, or they clash with it. Lowercase Win32 names (lstrcmpW...) stay.
        names = exports(dll)
        if stem in ("kernelbase", "ntdll"):
            names = [n for n in names if not (n.startswith("_") or (n[0].islower() and not n.startswith("lstr")))]
        deffile = os.path.join(args.outdir, stem + ".def")
        with open(deffile, "w") as f:
            f.write("LIBRARY %s\nEXPORTS\n" % base.upper())
            for n in names:
                f.write("    %s\n" % n)
        lib = os.path.join(args.outdir, stem + ".lib")
        subprocess.run([args.dlltool, "-m", "arm", "-d", deffile, "-l", lib], check=True)
        print("%s: %d exports -> %s" % (base, len(names), lib))
        if stem == "kernelbase":
            # WP8.1 has no KERNEL32.DLL nor ADVAPI32.DLL; WaitOnAddress & co. (synchronization.lib), the registry
            # and token functions live in KERNELBASE too: dependencies that link those libraries get KERNELBASE.DLL
            # imports.
            for alias in ("kernel32", "synchronization", "advapi32"):
                subprocess.run([args.dlltool, "-m", "arm", "-d", deffile, "-l", os.path.join(args.outdir, alias + ".lib")], check=True)


if __name__ == "__main__":
    sys.exit(main())
