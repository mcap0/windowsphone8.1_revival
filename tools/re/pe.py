# Minimal PE (ARM Thumb-2) helper: sections, MOV32T relocation xrefs, imports, .pdata functions.
import struct, bisect
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB


class PE:
    def __init__(s, path):
        d = s.d = open(path, 'rb').read()
        e = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, e + 6)[0]
        optsz = struct.unpack_from('<H', d, e + 20)[0]
        o = e + 24
        s.base = struct.unpack_from('<I', d, o + 28)[0]
        n = struct.unpack_from('<I', d, o + 92)[0]
        s.dirs = [struct.unpack_from('<II', d, o + 96 + 8 * i) for i in range(n)]
        s.secs = []
        so = o + optsz
        for i in range(nsec):
            name = d[so:so + 8].rstrip(b'\0').decode()
            vsz, va, rsz, rp = struct.unpack_from('<IIII', d, so + 8)
            s.secs.append((name, va, vsz, rp, rsz))
            so += 40
        s.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        s.relocs()
        s.imports()
        s.pdata()

    def off(s, rva):
        for name, va, vsz, rp, rsz in s.secs:
            if va <= rva < va + max(vsz, rsz):
                return rp + rva - va
        return None

    def rva_of_off(s, off):
        for name, va, vsz, rp, rsz in s.secs:
            if rp <= off < rp + rsz:
                return va + off - rp
        return None

    def wstr(s, rva, n=200):
        o = s.off(rva)
        out = []
        while len(out) < n:
            c = struct.unpack_from('<H', s.d, o)[0]
            if c == 0:
                break
            out.append(chr(c))
            o += 2
        return ''.join(out)

    def relocs(s):
        # site rva -> target rva, for MOVW/MOVT pairs (type 7) and absolute words (type 3)
        s.mov32, s.abs32 = {}, {}
        rva, size = s.dirs[5]
        p = rva
        while p < rva + size:
            page, bsz = struct.unpack_from('<II', s.d, s.off(p))
            if bsz == 0:
                break
            for k in range((bsz - 8) // 2):
                ent = struct.unpack_from('<H', s.d, s.off(p) + 8 + 2 * k)[0]
                t, at = ent >> 12, page + (ent & 0xFFF)
                o = s.off(at)
                if t == 7:
                    h = struct.unpack_from('<4H', s.d, o)
                    lo = s.imm16(h[0], h[1])
                    hi = s.imm16(h[2], h[3])
                    s.mov32[at] = ((hi << 16) | lo) - s.base
                elif t == 3:
                    s.abs32[at] = struct.unpack_from('<I', s.d, o)[0] - s.base
            p += bsz
        s.xref = {}
        for at, tg in list(s.mov32.items()) + list(s.abs32.items()):
            s.xref.setdefault(tg, []).append(at)

    @staticmethod
    def imm16(hw1, hw2):
        return ((hw1 & 0xF) << 12) | (((hw1 >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) | (hw2 & 0xFF)

    def imports(s):
        # IAT slot rva -> "dll!name"
        s.iat = {}
        rva, size = s.dirs[1]
        if not rva:
            return
        p = rva
        while True:
            ilt, _, _, name, iat = struct.unpack_from('<5I', s.d, s.off(p))
            if name == 0:
                break
            dll = s.d[s.off(name):s.d.index(b'\0', s.off(name))].decode()
            src = ilt or iat
            k = 0
            while True:
                v = struct.unpack_from('<I', s.d, s.off(src + 4 * k))[0]
                if v == 0:
                    break
                if v & 0x80000000:
                    nm = '#%d' % (v & 0xFFFF)
                else:
                    o = s.off(v + 2)
                    nm = s.d[o:s.d.index(b'\0', o)].decode()
                s.iat[iat + 4 * k] = dll + '!' + nm
                k += 1
            p += 20

    def pdata(s):
        # sorted function start rvas from the exception directory (ARM: 8-byte entries)
        rva, size = s.dirs[3]
        s.funcs = []
        for k in range(size // 8):
            b = struct.unpack_from('<I', s.d, s.off(rva + 8 * k))[0]
            s.funcs.append(b & ~1)
        s.funcs.sort()

    def func_of(s, rva):
        i = bisect.bisect_right(s.funcs, rva) - 1
        return s.funcs[i] if i >= 0 else None

    def func_end(s, start):
        i = bisect.bisect_right(s.funcs, start)
        return s.funcs[i] if i < len(s.funcs) else start + 0x400

    def dis(s, start, end):
        o = s.off(start)
        code = s.d[o:o + (end - start)]
        out = []
        for ins in s.md.disasm(code, start):
            note = ''
            if ins.address in s.mov32:
                note = s.describe(s.mov32[ins.address])
            elif ins.mnemonic.startswith('ldr') and '[pc, #' in ins.op_str:
                imm = int(ins.op_str.split('#')[1].rstrip(']'), 16)
                lit = ((ins.address + 4) & ~3) + imm
                if lit in s.abs32:
                    note = s.describe(s.abs32[lit])
                else:
                    o2 = s.off(lit)
                    note = '=0x%x' % struct.unpack_from('<I', s.d, o2)[0] if o2 is not None else ''
            out.append((ins.address, ins.mnemonic, ins.op_str, note))
        return out

    def describe(s, tg):
        if tg in s.iat:
            return '[IAT ' + s.iat[tg] + ']'
        o = s.off(tg)
        if o is None:
            return '-> %x' % tg
        b = s.d[o:o + 6]
        if len(b) == 6 and b[1] == 0 and b[3] == 0 and 32 <= b[0] < 127 and 32 <= b[2] < 127:
            return 'L"%s"' % s.wstr(tg, 90)
        if all(32 <= c < 127 for c in b[:4]):
            e = s.d.index(b'\0', o)
            return '"%s"' % s.d[o:min(e, o + 90)].decode('latin1')
        return '-> %x' % tg

    def find_wstr(s, text):
        pat = text.encode('utf-16-le')
        res, i = [], 0
        while True:
            i = s.d.find(pat, i)
            if i < 0:
                return res
            res.append(s.rva_of_off(i))
            i += 2
