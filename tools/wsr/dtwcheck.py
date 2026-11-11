"""dtwalign.py checked on MBROLA it4 (female) and it1 (male, other voice) against it3: vowel centres inside the true
vowel. python3 tools/wsr/dtwcheck.py [n_lines]"""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtwalign, vowels as V  # noqa: E402
from aligncheck import TEXT, PV  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
lines = [l.split('\t')[1].strip() for l in open(TEXT, encoding='utf-8')][:n]
for voice, speed in (('mb-it4', 160), ('mb-it4', 200), ('mb-it1', 130)):
    tot = inside = 0
    with tempfile.TemporaryDirectory() as d:
        for text in lines:
            got = V.synth(voice, text, speed, 1.0, d)
            if got is None:
                continue
            x, segs = got
            r = dtwalign.align(x, text)
            if r is None:
                continue
            al, _ = r
            for (ph, a, b), (tp, ta, tb) in zip(al, segs):
                if tp in PV:
                    c = (a + b) / 2 * 10 + 12.5
                    tot += 1
                    inside += ta <= c <= tb
    print(f'{voice} s{speed}: vowel centres inside the true vowel {inside}/{tot} = {100 * inside / max(tot, 1):.1f}%')
