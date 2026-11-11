# wwhere.py dll name...: like where.py for UTF-16 strings (each match, functions that reference it)
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pe import PE
p = PE(sys.argv[1])
for name in sys.argv[2:]:
    pat = name.encode('utf-16-le')
    i = p.d.find(pat); hits = 0
    while i >= 0:
        r = p.rva_of_off(i)
        if r is not None:
            fs = sorted(set(p.func_of(x) or 0 for x in p.xref.get(r, [])))
            if fs: print('%-40s str %x funcs %s' % (name, r, ' '.join('%x' % f for f in fs))); hits += 1
        i = p.d.find(pat, i + 2)
    if not hits: print(name, 'no refs')
