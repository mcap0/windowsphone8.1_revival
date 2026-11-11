# submitpatch.py in.dll out.dll [aumid]: the Bing app's -> (submit) opens our Risultati app with the search URL.
# Hooks HostedPlatformCM::NavigateInPlace (SearchCM.dll 8.1.14226, RVA 0x2b5f0, r1 = pszUrl) with a b.w into the
# .text slack (past VirtualSize, which is grown to cover it). The cave loads ShellChromeAPI.dll (kernelbase
# LoadLibraryExW via GetModuleHandleW + GetProcAddress, IAT slots read pc-relative) and calls
#   Shell_LaunchSessionGeneric(aumid, L"", pszUrl, 2)   (uri, tile id, launch arguments, flags)
# then returns S_OK without navigating (the hosted IE page never loads results anyway).
import struct, sys, hashlib
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pe import PE

HOOK = 0x2b5f0
IAT_GETMODULEHANDLEW, IAT_GETPROCADDRESS = 0x64070, 0x64074
src, dst = sys.argv[1], sys.argv[2]
aumid = sys.argv[3] if len(sys.argv) > 3 else 'DumbPhone.Risultati_ry999vsxp6fxe!App'
p = PE(src)
d = bytearray(p.d)
assert p.d[p.off(HOOK):p.off(HOOK) + 8] == bytes.fromhex('2de9f04f0df11c0b'), 'unexpected SearchCM.dll build'
assert p.iat[IAT_GETMODULEHANDLEW].endswith('!GetModuleHandleW') and p.iat[IAT_GETPROCADDRESS].endswith('!GetProcAddress')

# .text header: grow VirtualSize up to SizeOfRawData so the slack is mapped as code
e = struct.unpack_from('<I', d, 0x3c)[0]
sh = e + 24 + struct.unpack_from('<H', d, e + 20)[0]
assert d[sh:sh + 5] == b'.text'
vsz, va, rsz = struct.unpack_from('<III', d, sh + 8)
CAVE = (va + vsz + 15) & ~15
END = va + rsz
assert set(d[p.off(CAVE):p.off(END)]) == {0}
struct.pack_into('<I', d, sh + 8, rsz)

def h(v):
    return struct.pack('<H', v)

def movw(rd, imm):  # T3 MOVW
    i, imm4, imm3, imm8 = (imm >> 11) & 1, (imm >> 12) & 15, (imm >> 8) & 7, imm & 255
    return h(0xf240 | (i << 10) | imm4) + h((imm3 << 12) | (rd << 8) | imm8)

def bw(at, target):  # T4 B.W
    off = target - (at + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    return h(0xf000 | (s << 10) | ((off >> 12) & 0x3ff)) + h(0x9000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff))

class Asm:
    def __init__(s, base):
        s.base, s.code, s.fix = base, b'', []
    def at(s):
        return s.base + len(s.code)
    def emit(s, b):
        s.code += b
    def iat(s, slot):  # r3 = *(slot): movw r3, #delta ; add r3, pc ; ldr r3, [r3]
        add_at = s.at() + 4
        s.emit(movw(3, slot - (add_at + 4)))
        s.emit(h(0x447b) + h(0x681b))
    def adr(s, rd, label):
        s.fix.append(('adr', len(s.code), rd, label))
        s.emit(h(0))
    def cbz(s, rn, label):
        s.fix.append(('cbz', len(s.code), rn, label))
        s.emit(h(0))

a = Asm(CAVE)
a.emit(h(0xb51f))                        # push {r0-r4, lr}
a.emit(h(0x460c))                        # mov r4, r1 (pszUrl)
a.cbz(4, 'out')
a.iat(IAT_GETMODULEHANDLEW)
a.adr(0, 'kb')
a.emit(h(0x4798))                        # blx r3
a.cbz(0, 'out')
a.adr(1, 'lle')
a.iat(IAT_GETPROCADDRESS)
a.emit(h(0x4798))
a.cbz(0, 'out')
a.emit(h(0x4603))                        # mov r3, r0
a.adr(0, 'api')
a.emit(h(0x2100) + h(0x2200))            # movs r1, #0 ; movs r2, #0
a.emit(h(0x4798))                        # LoadLibraryExW(api, 0, 0)
a.cbz(0, 'out')
a.adr(1, 'proc')
a.iat(IAT_GETPROCADDRESS)
a.emit(h(0x4798))
a.cbz(0, 'out')
a.emit(h(0x4684))                        # mov ip, r0
a.adr(0, 'aumid')
a.adr(1, 'empty')
a.emit(h(0x4622))                        # mov r2, r4
a.emit(h(0x2302))                        # movs r3, #2
a.emit(h(0x47e0))                        # blx ip
labels = {'out': a.at()}
a.emit(h(0xe8bd) + h(0x401f))            # out: pop.w {r0-r4, lr}
a.emit(h(0x2000) + h(0x4770))            # movs r0, #0 ; bx lr

data = b''
def put(name, blob):
    global data
    while (a.at() + len(data)) % 4:
        data += b'\0'
    labels[name] = a.at() + len(data)
    data += blob
put('kb', 'kernelbase.dll\0'.encode('utf-16-le'))
put('lle', b'LoadLibraryExW\0')
put('api', 'ShellChromeAPI.dll\0'.encode('utf-16-le'))
put('proc', b'?Shell_LaunchSessionGeneric@@YAJPBG00I@Z\0')
put('aumid', (aumid + '\0').encode('utf-16-le'))
put('empty', b'\0\0')

code = bytearray(a.code)
for kind, off, reg, label in a.fix:
    at = CAVE + off
    if kind == 'adr':
        delta = labels[label] - ((at + 4) & ~3)
        assert 0 <= delta <= 1020 and delta % 4 == 0, (label, delta)
        code[off:off + 2] = h(0xa000 | (reg << 8) | (delta >> 2))
    else:
        delta = labels[label] - (at + 4)
        assert 0 <= delta <= 126 and delta % 2 == 0, (label, delta)
        code[off:off + 2] = h(0xb100 | (((delta >> 6) & 1) << 9) | (((delta >> 1) & 31) << 3) | reg)
blob = bytes(code) + data
assert CAVE + len(blob) <= END, hex(CAVE + len(blob))
d[p.off(CAVE):p.off(CAVE) + len(blob)] = blob
d[p.off(HOOK):p.off(HOOK) + 4] = bw(HOOK, CAVE)
open(dst, 'wb').write(d)
print(dst, hashlib.sha256(d).hexdigest(), 'cave %x: code %d + data %d bytes' % (CAVE, len(code), len(data)))
