# holdpatch.py in.dll out.dll [aumid]: long press of Search -> opens our Assistente instead of the speech UI.
# Rewrites SpeechToplevel::OnSearchPressAndHoldInvoked (ShSpeechUX.dll 8.1.14226, RVA 0x111fc) with
#   Shell_LaunchSessionGeneric(aumid, L"", L"", 2)  (as RetailDemoService launches apps; Shell_LaunchSessionByUri
#   with app://<id>/... only opens system apps), found via GetModuleHandleW + GetProcAddress, IAT slots read
#   pc-relative so no relocation is needed. Returns S_OK.
import struct, sys, hashlib
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pe import PE

FUNC, FREE_END = 0x111fc, 0x11334
IAT_GETMODULEHANDLEW, IAT_GETPROCADDRESS = 0x1d09c, 0x1d0a4
src, dst = sys.argv[1], sys.argv[2]
aumid = sys.argv[3] if len(sys.argv) > 3 else 'DumbPhone.Assistente_ry999vsxp6fxe!App'
p = PE(src)
d = bytearray(p.d)
assert p.d[p.off(FUNC):p.off(FUNC) + 4] == bytes.fromhex('2de9f04f'), 'unexpected ShSpeechUX.dll build'
assert p.rva_of_off(p.d.find(b'GetModuleHandleW\0')) is not None

def h(v):
    return struct.pack('<H', v)

def movw(rd, imm):  # T3 MOVW
    i, imm4, imm3, imm8 = (imm >> 11) & 1, (imm >> 12) & 15, (imm >> 8) & 7, imm & 255
    return h(0xf240 | (i << 10) | imm4) + h((imm3 << 12) | (rd << 8) | imm8)

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
    def adr(s, rd, label):  # adr rd, label (filled later)
        s.fix.append(('adr', len(s.code), rd, label))
        s.emit(h(0))
    def cbz(s, rn, label):
        s.fix.append(('cbz', len(s.code), rn, label))
        s.emit(h(0))

a = Asm(FUNC)
a.emit(h(0xb510))                        # push {r4, lr}
a.iat(IAT_GETMODULEHANDLEW)
a.adr(0, 'dll')
a.emit(h(0x4798))                        # blx r3
a.cbz(0, 'out')
a.adr(1, 'proc')
a.iat(IAT_GETPROCADDRESS)
a.emit(h(0x4798))                        # blx r3
a.cbz(0, 'out')
a.emit(h(0x4604))                        # mov r4, r0
a.adr(0, 'aumid')
a.adr(1, 'empty')
a.emit(h(0x460a))                        # mov r2, r1
a.emit(h(0x2302))                        # movs r3, #2
a.emit(h(0x47a0))                        # blx r4
labels = {'out': a.at()}
a.emit(h(0x2000) + h(0xbd10))            # out: movs r0, #0 ; pop {r4, pc}

data = b''
def put(name, blob):
    global data
    while (a.at() + len(data)) % 4:
        data += b'\0'
    labels[name] = a.at() + len(data)
    data += blob
put('dll', 'ShellChromeAPI.dll\0'.encode('utf-16-le'))
put('proc', b'?Shell_LaunchSessionGeneric@@YAJPBG00I@Z\0')
put('aumid', (aumid + '\0').encode('utf-16-le'))
put('empty', b'\0\0')

code = bytearray(a.code)
for kind, off, reg, label in a.fix:
    at = FUNC + off
    if kind == 'adr':
        delta = labels[label] - ((at + 4) & ~3)
        assert 0 <= delta <= 1020 and delta % 4 == 0, (label, delta)
        code[off:off + 2] = h(0xa000 | (reg << 8) | (delta >> 2))
    else:
        delta = labels[label] - (at + 4)
        assert 0 <= delta <= 126 and delta % 2 == 0, (label, delta)
        code[off:off + 2] = h(0xb100 | (((delta >> 6) & 1) << 9) | (((delta >> 1) & 31) << 3) | reg)
blob = bytes(code) + data
assert FUNC + len(blob) <= FREE_END, hex(FUNC + len(blob))
d[p.off(FUNC):p.off(FUNC) + len(blob)] = blob
open(dst, 'wb').write(d)
print(dst, hashlib.sha256(d).hexdigest(), 'code %d + data %d bytes' % (len(code), len(data)))
