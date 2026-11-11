#!/usr/bin/env python3
"""checkimports.py <pe>... [--package DIR]...

Checks that every DLL a PE imports exists on the phone (system32 of the MainOS dump, or one of the --package
directories shipped with the app) and that every imported function is exported by it. Exit status 1 when something
is missing: such a binary would fail to load on the Lumia."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from implib import exports  # noqa: E402
from peimports import imports  # noqa: E402

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSTEM32 = os.path.join(ENGINE, "..", "ref", "mainos", "fs", "Windows", "system32")


def apisets(path):
    """api-ms-win-* name (lowercase, no prefix/suffix) -> host DLL, from the phone's ApiSetSchema (version 4, 8.1)."""
    import struct
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    so = e + 24 + struct.unpack_from("<H", d, e + 20)[0]
    base = None
    for i in range(nsec):
        if d[so:so + 8].rstrip(b"\0") == b".apiset":
            base = struct.unpack_from("<I", d, so + 20)[0]
        so += 40
    version, size, flags, count = struct.unpack_from("<IIII", d, base)
    if version != 4:
        raise SystemExit("unsupported ApiSetSchema version %d" % version)
    hosts = {}
    for i in range(count):
        eflags, name_off, name_len, alias_off, alias_len, data_off = struct.unpack_from("<6I", d, base + 16 + 24 * i)
        name = d[base + name_off:base + name_off + name_len].decode("utf-16-le").lower()
        vflags, vcount = struct.unpack_from("<II", d, base + data_off)
        values = []
        for j in range(vcount):
            f, n_off, n_len, v_off, v_len = struct.unpack_from("<5I", d, base + data_off + 8 + 20 * j)
            values.append(d[base + v_off:base + v_off + v_len].decode("utf-16-le"))
        if values and values[0]:
            hosts[name] = values[0]
    return hosts


def resolve(dll, schema):
    """Host DLL of an API-set import (api-ms-win-core-synch-l1-2-0.dll -> kernelbase.dll), else the name itself."""
    name = dll.lower()
    if name.endswith(".dll"):
        name = name[:-4]
    for prefix in ("api-", "ext-"):
        if name.startswith(prefix) and name[len(prefix):] in schema:
            return schema[name[len(prefix):]]
    return dll


def index(directory):
    try:
        return {name.lower(): os.path.join(directory, name) for name in os.listdir(directory)}
    except FileNotFoundError:
        return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pe", nargs="+")
    ap.add_argument("--package", action="append", default=[], help="directory with DLLs shipped in the app package")
    args = ap.parse_args()

    places = [index(SYSTEM32)] + [index(d) for d in args.package]
    schema = apisets(os.path.join(SYSTEM32, "APISETSCHEMA.DLL"))
    cache = {}
    bad = 0
    for pe in args.pe:
        machine, result = imports(pe)
        for dll, functions in result:
            host = resolve(dll, schema)
            path = next((p[host.lower()] for p in places if host.lower() in p), None)
            if not path:
                print("%s: MISSING DLL %s (%d functions)" % (os.path.basename(pe), dll, len(functions)))
                bad += 1
                continue
            if path not in cache:
                cache[path] = set(exports(path))
            missing = [f for f in functions if not f.startswith("#") and f not in cache[path]]
            if missing:
                print("%s: %s lacks %s" % (os.path.basename(pe), dll, " ".join(sorted(missing))))
                bad += 1
            else:
                print("%s: %s ok (%d)%s" % (os.path.basename(pe), dll, len(functions), "" if host == dll else " -> " + host))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
