#!/usr/bin/env python3
# timeline.py [--file LOG] [--frames N]: the last frame timeline in the phone log ("gpu" in the address bar,
# Lumia::timelineMark in engine/port/Engine.cpp). One row per frame (b = render begins): what set it off and how long
# each step took, then medians.
#   p finger event (input thread)        q its handling on the scrolling thread
#   r composition asked by scrolling thread   m asked by main thread
#   t frame clock tick                   f requested frame started
#   b render begins   d draw done   w swap returned   c frame complete
#   g/h glFinish before the scene update (GPU still busy with the last frame)   u scene update done (tile uploads)
import argparse, os, re, statistics

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
parser = argparse.ArgumentParser()
parser.add_argument("--file", default=os.path.join(ROOT, "share/inbox/internet.txt"))
parser.add_argument("--frames", type=int, default=40)
args = parser.parse_args()

runs = []
for line in open(args.file, errors="replace"):
    m = re.search(r"linea: (\d+) eventi in \d+ s", line)
    if m:
        runs.append([])
        continue
    m = re.search(r"linea: (.*)", line)
    if m and runs:
        runs[-1] += [(float(t), k) for k, t in re.findall(r"([a-z])(-?[\d.]+)", m.group(1))]
if not runs:
    raise SystemExit("nessuna linea nel log: scrivi \"gpu\" nella barra degli indirizzi e scorri")
events = runs[-1]
print(f"{len(events)} eventi")


def last_before(kind, t, after=None):
    best = None
    for et, k in events:
        if et > t:
            break
        if k == kind and (after is None or et >= after):
            best = et
    return best


begins = [t for t, k in events if k == "b"]
rows = []
for i, b in enumerate(begins):
    prev_c = last_before("c", b)
    d = next((t for t, k in events if k == "d" and t >= b), None)
    w = next((t for t, k in events if k == "w" and t >= b), None)
    c = next((t for t, k in events if k == "c" and t >= b), None)
    window = begins[i - 1] if i else b - 50
    trig = [(t, k) for t, k in events if window < t <= b and k in "rmtf"]
    p = last_before("p", b)
    nxt = lambda kind: next((t for t, k in events if k == kind and t >= b and (d is None or t <= d)), None)
    g, h, u = nxt("g"), nxt("h"), nxt("u")
    rows.append(dict(b=b, interval=b - begins[i - 1] if i else None, draw=d - b if d else None,
                     swap=w - d if w and d else None, idle=b - prev_c if prev_c else None,
                     trig="".join(k for _, k in trig), lastr=(b - max((t for t, k in trig if k == "r"), default=b - 999)),
                     finger=(b - p) if p else None,
                     gpu=(h - g) if g is not None and h is not None else None,
                     upload=(u - h) if u is not None and h is not None else None))

fmt = lambda v: f"{v:6.1f}" if v is not None else "     -"
print("   inizio  interv  b->d  swap  GPU occupata  upload  attesa(c->b)  dito->b  r->b  cause")
for r in rows[:args.frames]:
    print(f"  {r['b']:7.1f} {fmt(r['interval'])} {fmt(r['draw'])} {fmt(r['swap'])}  {fmt(r['gpu'])}    {fmt(r['upload'])}     {fmt(r['idle'])}     {fmt(r['finger'])} {fmt(r['lastr'] if r['lastr'] < 500 else None)}  {r['trig']}")


def med(key):
    v = [r[key] for r in rows if r[key] is not None]
    return f"mediana {statistics.median(v):.1f} p90 {sorted(v)[int(len(v) * 0.9)]:.1f}" if v else "-"


print(f"\nfotogrammi {len(rows)}: intervallo {med('interval')}, disegno {med('draw')}, swap {med('swap')}, "
      f"fermo tra fine e inizio {med('idle')}\n  GPU ancora occupata a inizio fotogramma {med('gpu')}, upload dei tile {med('upload')}")
counts = {k: sum(1 for _, kk in events if kk == k) for k in "pqrmtfbghudwc"}
print("conteggi: " + ", ".join(f"{k}={n}" for k, n in counts.items()))
