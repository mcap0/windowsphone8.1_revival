#!/usr/bin/env python3
# sym.py <webkit.map> <offset>...: the function at each webkit.dll offset from the phone log (preferred base 0x10000000).
import sys,re,bisect,subprocess
syms=[]
for line in open(sys.argv[1], errors='replace'):
    m=re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(\S+)',line)
    if m: syms.append((int(m.group(2),16),m.group(1),m.group(3)))
syms.sort(); addrs=[a for a,_,_ in syms]
for o in sys.argv[2:]:
    v=0x10000000+int(o,16); i=bisect.bisect_right(addrs,v)-1
    a,n,obj=syms[i]
    d=subprocess.run(['llvm-undname',n],capture_output=True,text=True).stdout.strip().splitlines()
    print(o,'= +'+hex(v-a),'in',(d[-1] if d else n)[:260],'['+obj+']')
