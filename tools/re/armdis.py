import sys
sys.path.insert(0, '.')
from pe import PE
p = PE(sys.argv[1])
start = int(sys.argv[2], 16)
start = p.func_of(start) if len(sys.argv) < 4 else start
end = p.func_end(start) if len(sys.argv) < 4 else int(sys.argv[3], 16)
print('func %x-%x callers:' % (start, end))
for a, m, o, n in p.dis(start, end):
    if m in ('bl', 'blx') and o.startswith('#'):
        tg = int(o[1:], 16)
        n = n or ('f_%x' % tg)
    print('%6x  %-8s %-28s %s' % (a, m, o, n))
