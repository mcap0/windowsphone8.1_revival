#!/usr/bin/env python3
# profile.py [--file LOG] [--map MAP] [--all]: the last profile of the Web thread in the phone log ("profilo" in the
# address bar, engine/port/Profiler.cpp), with function names from webkit.dll's link map. --all prints every profile.
import argparse, bisect, os, re, subprocess

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BASE = 0x10000000  # webkit.dll's preferred base in the map

parser = argparse.ArgumentParser()
parser.add_argument("--file", default=os.path.join(ROOT, "share/inbox/internet.txt"))
parser.add_argument("--map", default=os.path.join(ROOT, "engine/out/webkit/bin/webkit.map"))
parser.add_argument("--all", action="store_true")
args = parser.parse_args()

symbols = []
for line in open(args.map, errors="replace"):
    m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(\S+)", line)
    if m:
        symbols.append((int(m.group(2), 16), m.group(1), m.group(3)))
symbols.sort()
addresses = [a for a, _, _ in symbols]

# One profile = a header line and the "proprio"/"incluso" lines after it.
profiles = []
for line in open(args.file, errors="replace"):
    if "profilo: " in line and "campioni" in line:
        profiles.append({"header": line.split("motore: ", 1)[-1].strip(), "proprio": [], "incluso": [], "chiamante": []})
    m = re.search(r"profilo (proprio|incluso|chiamante):(.*)", line)
    if m and profiles:
        profiles[-1][m.group(1)] += [(int(o, 16), float(p)) for o, p in re.findall(r"([0-9A-F]+) ([\d.]+)%", m.group(2))]
if not profiles:
    raise SystemExit("nessun profilo nel log: scrivi \"profilo\" nella barra degli indirizzi")

offsets = sorted({o for p in profiles for kind in ("proprio", "incluso", "chiamante") for o, _ in p[kind]})
mangled = {}
for o in offsets:
    i = bisect.bisect_right(addresses, BASE + o) - 1
    mangled[o] = (symbols[i][1], symbols[i][2]) if i >= 0 else ("?", "")
names = list({n for n, _ in mangled.values()})
demangled = {}
# llvm-undname prints "<mangled>\n<demangled>\n\n" per input line.
out = subprocess.run(["llvm-undname"], input="\n".join(names) + "\n", capture_output=True, text=True).stdout
for block in out.strip().split("\n\n"):
    lines = block.splitlines()
    if len(lines) >= 2:
        demangled[lines[0]] = lines[1]


def name(o):
    n, obj = mangled[o]
    text = demangled.get(n) or n
    text = re.sub(r"\b(public|private|protected): |\b(virtual|static) |__cdecl |class |struct ", "", text)
    return f"{text[:150]}  [{obj.split(':')[0]}]"


for p in (profiles if args.all else profiles[-1:]):
    print(p["header"])
    for kind, title in (("proprio", "tempo proprio (la funzione stessa)"), ("incluso", "tempo incluso (con le chiamate)"),
                        ("chiamante", "chi aspettava, chiamava il sistema o eseguiva JS (funzione webkit.dll più vicina)")):
        print(f"  {title}:")
        for o, percent in p[kind]:
            print(f"    {percent:5.1f}%  {name(o)}")
