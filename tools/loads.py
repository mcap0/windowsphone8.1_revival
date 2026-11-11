#!/usr/bin/env python3
"""loads.py — one line per page load in share/inbox/internet.txt (the log of the app "Internet", apps/Browser).

For each app run (a run starts at the `--- avvio` line) and each main-frame load in it (`caricamento: inizio` to
`caricamento: fine`/`errore`): the URL, how long the load took, and where the Web thread's time went meanwhile, summed
from the 2 s activity lines (`attivita': [web] occupato ...`) that fall inside the load. Bytecode cache lines are
counted per load: files read from the disk (`dal disco`, with the tracer on) and writes (`scrivo`).

It is the reader of the phase 5 bench (PLAN.md §56): the same pages, cold and warm, before and after a change.

Usage:
    tools/loads.py                 # last 3 runs
    tools/loads.py --runs 6
    tools/loads.py --file other.txt --all
    tools/loads.py --filter gemini # only loads whose URL contains this
    tools/loads.py --banco         # the last 3 runs of the "banco" bench, side by side
    tools/loads.py --banco --runs 1
"""
import argparse
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
LABELS = ("js", "style", "layout", "parser", "timer", "render", "port")

parser = argparse.ArgumentParser()
parser.add_argument("--file", default=os.path.join(ROOT, "share/inbox/internet.txt"))
parser.add_argument("--runs", type=int, default=3)
parser.add_argument("--all", action="store_true")
parser.add_argument("--filter", default="")
parser.add_argument("--banco", action="store_true")
args = parser.parse_args()


def new_load(t):
    return {"start": t, "end": None, "url": "", "outcome": "", "busy": 0.0, "window": 0.0,
            "labels": dict.fromkeys(LABELS, 0.0), "hits": 0, "hitKB": 0, "writes": 0, "writeKB": 0}


runs = []
for line in open(args.file, errors="replace"):
    line = line.rstrip("\n")
    m = re.match(r"\s*(\d+) ms (.*)", line)
    if not m:
        continue
    t, text = int(m.group(1)), m.group(2)
    if text.startswith("--- avvio"):
        runs.append({"loads": [], "current": None, "seen": set()})
        continue
    if not runs:
        continue
    run = runs[-1]
    # The log sender may deliver a line twice.
    key = (t, text[:160])
    if key in run["seen"]:
        continue
    run["seen"].add(key)
    bench = run.setdefault("bench", [])
    bm = re.match(r"banco: (\d+) (\S+)", text)
    if bm:
        bench.append({"round": int(bm.group(1)), "url": bm.group(2), "start": t, "end": None, "activity": []})
        if run.get("benches"):
            run["benches"][-1].append(bench[-1])
    elif text.startswith("banco: inizio"):
        run.setdefault("benches", []).append([])
    elif bench and re.match(r"banco: (fine|fermato)", text):
        bench.append(None)
    elif bench and bench[-1]:
        if text.startswith(("caricamento: fine", "caricamento: errore")):
            bench[-1]["end"] = t
        wm = re.search(r"attivita': \[web\] occupato ([\d.]+)/([\d.]+) ms", text)
        if wm:
            bj = re.search(r"\bjs ([\d.]+)", text.split("lunghi", 1)[-1])
            bench[-1]["activity"].append((t, float(wm.group(1)), float(wm.group(2)), float(bj.group(1)) if bj else 0.0))
    load = run["current"]
    if text.startswith("caricamento: inizio"):
        if load and load["end"] is None:
            load["end"], load["outcome"] = t, "sostituito"
        run["current"] = new_load(t)
        run["loads"].append(run["current"])
        continue
    if not load:
        continue
    if text.startswith("url: ") and not load["url"]:
        load["url"] = text[5:]
    elif text.startswith("caricamento: fine") and load["end"] is None:
        load["end"], load["outcome"] = t, "fine"
    elif text.startswith("caricamento: errore") and load["end"] is None:
        load["end"], load["outcome"] = t, "errore"
        if not load["url"]:
            load["url"] = text.split(" ", 4)[-1]
    elif "bytecode: dal disco" in text and load["end"] is None:
        load["hits"] += 1
        hm = re.search(r"dal disco (\d+) KB", text)
        load["hitKB"] += int(hm.group(1)) if hm else 0
    elif "bytecode: scrivo" in text:
        load["writes"] += 1
        wm = re.search(r"(\d+) KB", text)
        load["writeKB"] += int(wm.group(1)) if wm else 0
    am = re.search(r"attivita': \[web\] occupato ([\d.]+)/([\d.]+) ms", text)
    # A summary covers the 2 s before it: it counts for the load if most of that window is inside it.
    if am and (load["end"] is None or t <= load["end"] + 1000):
        load["busy"] += float(am.group(1))
        load["window"] += float(am.group(2))
        for label in LABELS:
            lm = re.search(r"\b%s ([\d.]+)" % label, text.split("lunghi", 1)[-1])
            if lm:
                load["labels"][label] += float(lm.group(1))



def bench_row(item):
    """Load time, Web thread and JS during the load (to the last load end), share of the Web thread busy at rest after."""
    if item["end"] is None:
        return None
    during = [a for a in item["activity"] if a[0] <= item["end"] + 1000]
    rest = [a for a in item["activity"] if a[0] > item["end"] + 2000]
    return ((item["end"] - item["start"]) / 1000, sum(a[1] for a in during) / 1000, sum(a[3] for a in during) / 1000,
            100 * sum(a[1] for a in rest) / sum(a[2] for a in rest) if rest else None)


if args.banco:
    done = [b for r in runs for b in r.get("benches", []) if b][-args.runs:]
    for items in done:
        print("=== banco, prima pagina a %.0f s" % (items[0]["start"] / 1000))
    urls = list(dict.fromkeys(i["url"] for items in done for i in items))
    print(f"{'giro':>4} {'durata':>7} {'web':>7} {'js':>7} {'riposo':>7} |" * len(done) + "  pagina")
    for url in urls:
        for wanted in (1, 2):
            cells = []
            for items in done:
                item = next((i for i in items if i["url"] == url and i["round"] == wanted), None)
                row = bench_row(item) if item else None
                if row:
                    idle = f"{row[3]:.0f}%" if row[3] is not None else "-"
                    cells.append(f"{wanted:>4} {row[0]:6.1f}s {row[1]:6.1f}s {row[2]:6.1f}s {idle:>7} |")
                else:
                    cells.append(f"{wanted:>4} {'-':>7} {'-':>7} {'-':>7} {'-':>7} |")
            print("".join(cells) + "  " + url[:60])
    raise SystemExit

selected = runs if args.all else runs[-args.runs:]
first = len(runs) - len(selected) + 1
for index, run in enumerate(selected, first):
    loads = [l for l in run["loads"] if args.filter in l["url"]]
    if not loads:
        continue
    print(f"=== esecuzione {index}")
    print(f"{'inizio':>8} {'durata':>8}  {'occupato':>9} " + " ".join(f"{l:>7}" for l in LABELS) + "  bytecode       url")
    for l in loads:
        duration = f"{(l['end'] - l['start']) / 1000:.1f} s" if l["end"] is not None else "aperto"
        busy = f"{l['busy'] / 1000:.1f} s" if l["window"] else "-"
        labels = " ".join(f"{l['labels'][k] / 1000:7.1f}" for k in LABELS)
        bytecode = f"{l['hits']} letti {l['hitKB']} KB, {l['writes']} scritture {l['writeKB']} KB" if l["hits"] or l["writes"] else ""
        url = l["url"][:70] + ("" if l["outcome"] in ("fine", "") else f" ({l['outcome']})")
        print(f"{l['start'] / 1000:7.1f}s {duration:>8}  {busy:>9} {labels}  {bytecode:<14} {url}")
