#!/usr/bin/env python3
"""sym.py — resolves the webkit.dll offsets printed by the app's crash handlers to symbols.

The app reports `webkit.dll+0xXXXXXX` (see MainPage.xaml.cpp: address - module base), which is a **RVA**, while
the linker map lists publics as `section:offset`. The two differ by the section's RVA (0x1000 for .text in our
build), and getting it wrong silently lands one page early on a plausible-looking wrong symbol. The section
offset is therefore read from the PE headers of the dll, never assumed.

Usage:
    tools/sym.py 0x239AF5                 # one offset (0x optional)
    tools/sym.py 239AF5 239AE3            # several
    tools/sym.py --stack "1D5984 1D6800"  # the whole list from an ABORT / ECCEZIONE line
    tools/sym.py --dll path --map path 0x239AF5
"""
import argparse
import bisect
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DEFAULT_DLL = os.path.join(ROOT, "engine", "out", "webkit", "bin", "webkit.dll")
DEFAULT_MAP = os.path.join(ROOT, "engine", "out", "webkit", "bin", "webkit.map")


def pe_sections(path):
    """{name: virtual address} from the PE's section table."""
    with open(path, "rb") as f:
        data = f.read(4096)
    if data[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE file")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: not a PE file")
    coff = e_lfanew + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    table = coff + 20 + opt_size
    sections = {}
    for i in range(n_sections):
        entry = table + 40 * i
        name = data[entry:entry + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_address = struct.unpack_from("<II", data, entry + 8)
        sections[name] = (virtual_address, virtual_size)
    return sections


def load_publics(map_path, text_rva, text_size):
    """[(section_offset, mangled_name)] in .text, sorted. The map is 'Address  Publics by Value  Rva+Base  Lib:Object'."""
    publics = []
    started = False
    address_re = re.compile(r"^\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)")
    with open(map_path, errors="replace") as f:
        for line in f:
            if not started:
                if "Publics by Value" in line:
                    started = True
                continue
            m = address_re.match(line)
            if not m:
                continue
            section = int(m.group(1), 16)
            offset = int(m.group(2), 16)
            # Section 1 is .text in every map our linker writes; keep only symbols inside it.
            if section == 1 and offset < text_size:
                publics.append((offset, m.group(3)))
    publics.sort()
    return publics


def demangle(names):
    """c++filt on the whole batch: one process instead of one per symbol."""
    if not names:
        return []
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=False)
        result = out.stdout.splitlines()
        return result if len(result) == len(names) else names
    except FileNotFoundError:
        return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("offsets", nargs="*", help="webkit.dll offsets (RVA), with or without 0x")
    parser.add_argument("--stack", help="space-separated list of offsets, as printed after 'pila'")
    parser.add_argument("--dll", default=DEFAULT_DLL)
    parser.add_argument("--map", default=DEFAULT_MAP)
    parser.add_argument("--no-demangle", action="store_true")
    args = parser.parse_args()

    raw = list(args.offsets)
    if args.stack:
        raw += args.stack.replace(",", " ").split()
    if not raw:
        parser.error("no offsets given")

    sections = pe_sections(args.dll)
    text_rva, text_size = sections.get(".text", (0x1000, 0))
    publics = load_publics(args.map, text_rva, text_size)
    if not publics:
        raise SystemExit(f"{args.map}: no publics found")
    offsets = [entry[0] for entry in publics]

    unresolved = []
    for token in raw:
        try:
            rva = int(token, 16)
        except ValueError:
            print(f"{token}: not a hex offset")
            continue
        section_offset = rva - text_rva
        index = bisect.bisect_right(offsets, section_offset) - 1
        if index < 0:
            print(f"{token}: 0x{rva:X} is before the first public in .text")
            continue
        name = publics[index][1]
        delta = section_offset - publics[index][0]
        unresolved.append((token, rva, name, delta))

    names = [entry[2] for entry in unresolved]
    pretty = names if args.no_demangle else demangle(names)
    for (token, rva, _, delta), name in zip(unresolved, pretty):
        print(f"0x{token.lstrip('0xX'):>8}: RVA 0x{rva:07X}  offset sezione 0x{rva - text_rva:07X}  {name} + 0x{delta:X}")


if __name__ == "__main__":
    main()
