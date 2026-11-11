import sys
sys.path.insert(0, '.')
from pe import PE
p = PE(sys.argv[1])
for name in sys.argv[2:]:
    pat = name.encode() + b'\0'
    i = p.d.find(pat)
    while i >= 0 and p.d[i - 1:i] not in (b'\0',):
        i = p.d.find(pat, i + 1)
    if i < 0:
        print(name, 'not found'); continue
    r = p.rva_of_off(i)
    fs = sorted(set(p.func_of(x) or 0 for x in p.xref.get(r, [])))
    print('%-50s str %x funcs %s' % (name, r, ' '.join('%x' % f for f in fs)))
