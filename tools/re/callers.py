# callers.py dll addr... : direct bl/b.w callers and data refs (vtables) of each address
import sys, pickle, os
sys.path.insert(0, '.')
from pe import PE
p = PE(sys.argv[1])
cache = sys.argv[1] + '.calls'
if os.path.exists(cache):
    calls = pickle.load(open(cache, 'rb'))
else:
    calls = {}
    for i, f in enumerate(p.funcs):
        end = p.funcs[i + 1] if i + 1 < len(p.funcs) else f + 0x200
        if end - f > 0x8000: end = f + 0x8000
        for a, m, o, n in p.dis(f, end):
            if m in ('bl', 'blx', 'b.w', 'b') and o.startswith('#'):
                calls.setdefault(int(o[1:], 16), []).append(a)
    pickle.dump(calls, open(cache, 'wb'))
for x in sys.argv[2:]:
    t = int(x, 16)
    c = calls.get(t, [])
    print('%x: calls from %s' % (t, ' '.join('%x(f%x)' % (a, p.func_of(a)) for a in c[:20])))
    refs = [a for a, v in p.abs32.items() if (v & ~1) == t]
    print('   data refs %s' % ' '.join('%x' % a for a in refs[:20]))
