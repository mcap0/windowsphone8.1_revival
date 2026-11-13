#!/usr/bin/env python3
"""armprof.py [--top N] [--save FILE] [--load FILE] -- PROGRAM [ARGS...] -- where a Linux ARMv7 program (the jsc of
engine/out/jsc-linux, the libbench builds) spends its instructions, off the phone (PLAN.md section 77). Runs it under the
plugin-capable qemu-arm of engine/out/qemu-plugin with armprof.c, which counts the instructions each translated block
executes (all threads), then attributes them to functions: the program's own symbols, the shared libraries it mapped
(symbols from engine/out/jsc-linux/sysroot), and "JIT code" for executable memory without a file.
The counts are qemu's guest instructions: a deterministic measure of work, not the phone's cycles.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys
import tempfile

ENGINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
QEMU = os.path.join(ENGINE, 'out', 'qemu-plugin')
SYSROOT = os.path.join(ENGINE, 'out', 'jsc-linux', 'sysroot')
PLUGIN_SOURCE = os.path.join(ENGINE, 'tools', 'armprof', 'armprof.c')
PLUGIN = os.path.join(QEMU, 'libarmprof.so')
# qemu's source tree (for include/qemu/qemu-plugin.h) and a glib-2.0 pkg-config directory: the environment's, else ours.
QEMU_SOURCE = os.environ.get('QEMU_SOURCE') or os.path.expanduser('~/Workspace/emulation/tools/qemu-10.1.2')
GLIB_PKGCONFIG = os.environ.get('GLIB_PKGCONFIG') or os.path.expanduser('~/App/miniforge3/envs/fwre/lib/pkgconfig')


def build_plugin():
    if os.path.exists(PLUGIN) and os.path.getmtime(PLUGIN) >= os.path.getmtime(PLUGIN_SOURCE):
        return
    cflags = subprocess.run(['pkg-config', '--cflags', 'glib-2.0'], capture_output=True, text=True, check=True,
                            env=dict(os.environ, PKG_CONFIG_PATH=GLIB_PKGCONFIG)).stdout.split()
    subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-I', os.path.join(QEMU_SOURCE, 'include', 'qemu')] + cflags +
                   [PLUGIN_SOURCE, '-o', PLUGIN], check=True)


class Symbols:
    """Function symbols of one ELF file, by address."""

    def __init__(self, path, dynamic=False):
        self.addresses, self.names = [], []
        entries = {}
        for flags in ([], ['-D']):
            result = subprocess.run(['llvm-nm', '-n', '-C', '--defined-only'] + flags + [path], capture_output=True,
                                    text=True)
            for line in result.stdout.splitlines():
                m = re.match(r'([0-9a-f]+) ([tTwW]) (.*)', line)
                if m:
                    address = int(m.group(1), 16) & ~1  # Thumb functions have the low bit set
                    entries.setdefault(address, m.group(3))
        for address in sorted(entries):
            self.addresses.append(address)
            self.names.append(entries[address])
        self.segments = []
        result = subprocess.run(['readelf', '-lW', path], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            m = re.match(r'\s*LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+?)\s+0x[0-9a-f]+\s*$',
                         line)
            if m:
                self.segments.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5)))

    def name(self, address):
        i = bisect.bisect_right(self.addresses, address) - 1
        return self.names[i] if i >= 0 else '?'


def short(name, width=150):
    name = re.sub(r'\(.*\)', '()', name) if len(name) > width else name
    return name if len(name) <= width else name[:width - 3] + '...'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--top', type=int, default=40)
    parser.add_argument('--save', help='keep the raw counts in this file')
    parser.add_argument('--load', help='profile an earlier --save instead of running (PROGRAM names its binary)')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('no program')
    raw = args.load or args.save or tempfile.mktemp(suffix='.armprof')
    if not args.load:
        build_plugin()
        run = subprocess.run([os.path.join(QEMU, 'b2', 'qemu-arm'), '-L', SYSROOT, '-plugin', f'{PLUGIN},out={raw}']
                             + command)
        if run.returncode:
            print(f'(the program exited with {run.returncode})', file=sys.stderr)
    program = Symbols(command[0])
    libraries = []
    totals = collections.Counter()
    blocks = []
    for line in open(raw):
        parts = line.split()
        if parts[0] == 'code':
            continue
        if parts[0] == 'map':
            start, end, offset, path = int(parts[1], 16), int(parts[2], 16), int(parts[3], 16), parts[4]
            local = os.path.join(SYSROOT, path.lstrip('/'))
            if os.path.exists(local):
                libraries.append((start, end, offset, os.path.basename(path), Symbols(os.path.realpath(local))))
            continue
        blocks.append((int(parts[0], 16), int(parts[1]) * int(parts[2])))
    text = [(vaddr, vaddr + size) for offset, vaddr, size, flags in program.segments if 'E' in flags]
    total = 0
    for pc, count in blocks:
        total += count
        if any(start <= pc < end for start, end in text):
            totals[short(program.name(pc))] += count
            continue
        for start, end, offset, name, symbols in libraries:
            if start <= pc < end:
                file_offset = pc - start + offset
                vaddr = next((v + file_offset - o for o, v, size, flags in symbols.segments
                              if o <= file_offset < o + size), file_offset)
                totals[f'{short(symbols.name(vaddr))} [{name}]'] += count
                break
        else:
            totals['JIT code (or the dynamic loader)'] += count
    print(f'{total / 1e6:.1f} M instructions')
    for name, count in totals.most_common(args.top):
        print(f'{count * 100 / total:6.2f}%  {count / 1e6:9.1f} M  {name}')
    if not args.save and not args.load:
        os.remove(raw)


if __name__ == '__main__':
    main()
