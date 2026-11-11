#!/usr/bin/env python3
"""trace.py — numbers out of share/inbox/internet.txt (the log of the app "Internet", apps/Browser).

One block per app run (a run starts at the `--- avvio` line), with the four measurements that decisions are made on:

  frame   interval between presented frames (compositor), split into paint and swap so a slow frame is attributed
          before anything is changed. Current builds log one `compositor: 2.0 s, N fotogrammi, intervallo mediana ...`
          summary every 2 s plus a `fotogramma lento` line per janky frame (port/compositor/FrameStats.h); older logs
          have one `fotogramma N, ...` line per frame. Both are read.
  tap     `tap: eseguito X ms dopo l'accodamento`: how long the tap waited in the Web thread's queue (input lane).
  pan     finger timestamp -> scrolling thread dispatch, must stay ~0: one `lw pan: gesto di N eventi, ritardo dal dito
          mediana X p90 Y max Z ms` line per gesture (older logs: one `lw pan N ...: X ms dopo il dito` per event).
          Only meaningful from the build where MonotonicTime is QPC (2026-10-27): before, the finger timestamp never
          matched the engine clock and the engine silently used "now", so every old value is ~0 by construction.
  crash   ABORT / ECCEZIONE lines verbatim: symbolise them with tools/sym.py against the map of the same build.
  attivita  F2.c: `attivita': [web] task X ms (coda i/n/l) label T ...` for every Web thread task over 30 ms, and
          `attivita': [web] occupato X/Y ms (Z%), task N, lunghi M label T ...` every 2 s. The labels say where the time
          went inside the task (js, style, layout, parser, timer, render, port); the queue depths say what was
          waiting behind it. Only the thread the port names is measured, so every line is the Web thread.

Usage:
    tools/trace.py                  # last 2 runs
    tools/trace.py --runs 5
    tools/trace.py --file other.txt --runs 1
    tools/trace.py --slow            # also list the 5 slowest frames of each run
"""
import argparse
import os
import re
import statistics
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DEFAULT_FILE = os.path.join(ROOT, "share", "inbox", "internet.txt")

STAMP = re.compile(r"^\s*(\d+) ms\s")
RUN_START = re.compile(r"^\s*(\d+) ms --- avvio")
FRAME = re.compile(r"fotogramma (lento )?(\d+), ([\d.]+) ms dal precedente, pittura ([\d.]+), swap ([\d.]+)")
FRAME_SUMMARY = re.compile(r"compositor: ([\d.]+) s, (\d+) fotogrammi, intervallo mediana ([\d.]+) p90 ([\d.]+) max ([\d.]+) ms, "
                           r"vsync 1/2/3\+ (\d+)/(\d+)/(\d+)%, oltre 50 ms (\d+), pittura mediana ([\d.]+) p90 ([\d.]+) "
                           r"max ([\d.]+), swap mediana ([\d.]+) p90 ([\d.]+) max ([\d.]+), dopo pausa (\d+)"
                           r"(?:, tick (\d+))?"
                           r"(?:, aggiornamento mediana ([\d.]+) p90 ([\d.]+), disegno mediana ([\d.]+) p90 ([\d.]+))?")
PAN_GESTURE = re.compile(r"lw pan: gesto di (\d+) eventi.*mediana ([\d.]+) p90 ([\d.]+) max ([\d.]+) ms")
TAP = re.compile(r"tap: eseguito ([\d.]+) ms")
CLICK = re.compile(r"tap: click dopo ([\d.]+) ms dal dito")
PAN = re.compile(r"lw pan \d+ a .*?: ([\d.]+) ms")
PICTURE = re.compile(r"picture del layer (\d+)x(\d+) px.*registrata in ([\d.]+) ms")
URL = re.compile(r"url: (\S+)")
TITLE = re.compile(r"titolo: (.+)$")
ACTIVITY_TASK = re.compile(r"attivita': \[(\w+)\] task ([\d.]+) ms \(coda (\d+)/(\d+)/(\d+)\)(.*)$")
ACTIVITY_SUMMARY = re.compile(r"attivita': \[(\w+)\] occupato ([\d.]+)/(\d+) ms \(([\d.]+)%\), task (\d+), lunghi (\d+)(.*)$")
LABEL = re.compile(r"(\w+) ([\d.]+)")
CRASH = ("ABORT", "ECCEZIONE")


def split_runs(lines):
    """[(start_line_index, [lines])] — a run begins where the app prints `--- avvio`.

    At startup the app replays the previous run verbatim between `--- esecuzione precedente` and `--- fine esecuzione
    precedente`; that copy also contains an `--- avvio` line, and counting it as a new run would merge two runs into
    one block and put the dump's frames in the wrong run.
    """
    starts = []
    inside_dump = False
    for i, line in enumerate(lines):
        if "--- esecuzione precedente" in line:
            inside_dump = True
            continue
        if "--- fine esecuzione precedente" in line:
            inside_dump = False
            continue
        if not inside_dump and RUN_START.match(line):
            starts.append(i)
    if not starts:
        return [(0, lines)]
    runs = []
    for n, start in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        runs.append((start, lines[start:end]))
    return runs


def quantile(sorted_values, p):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(p * len(sorted_values)))]


def summarize(name, values, unit="ms"):
    if not values:
        return f"{name}: n=0"
    ordered = sorted(values)
    return (f"{name}: n={len(ordered):<5} mediana {statistics.median(ordered):6.1f}  "
            f"p90 {quantile(ordered, 0.9):6.1f}  max {ordered[-1]:8.1f} {unit}")


def block(index, lines, slow):
    frames = []
    slow_frames = []
    for line in lines:
        m = FRAME.search(line)
        if m:
            frame = (float(m.group(3)), float(m.group(4)), float(m.group(5)), int(m.group(2)))
            (slow_frames if m.group(1) else frames).append(frame)
    summaries = [m for m in (FRAME_SUMMARY.search(line) for line in lines) if m]
    gestures = [(int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)))
                for m in (PAN_GESTURE.search(line) for line in lines) if m]
    taps = [float(TAP.search(line).group(1)) for line in lines if TAP.search(line)]
    clicks = [float(CLICK.search(line).group(1)) for line in lines if CLICK.search(line)]
    pans = [float(PAN.search(line).group(1)) for line in lines if PAN.search(line)]
    pictures = [(int(PICTURE.search(line).group(1)), int(PICTURE.search(line).group(2)),
                 float(PICTURE.search(line).group(3))) for line in lines if PICTURE.search(line)]
    urls = [URL.search(line).group(1) for line in lines if URL.search(line)]
    titles = [TITLE.search(line).group(1).strip() for line in lines if TITLE.search(line)]
    tasks = [(float(m.group(2)), (int(m.group(3)), int(m.group(4)), int(m.group(5))),
              dict((name, float(ms)) for name, ms in LABEL.findall(m.group(6))))
             for m in (ACTIVITY_TASK.search(line) for line in lines) if m]
    windows = [(float(m.group(2)), int(m.group(3)), float(m.group(4)), int(m.group(5)), int(m.group(6)))
               for m in (ACTIVITY_SUMMARY.search(line) for line in lines) if m]
    crashes = [line.strip() for line in lines if line.strip() and any(c in line for c in CRASH)]

    print(f"--- run {index + 1}  ({len(lines)} righe)")
    if titles:
        print(f"    titolo: {titles[-1]}")
    if urls:
        print(f"    url:    {urls[-1]}")
    if not frames and not summaries and not taps and not pans and not gestures:
        print("    (nessuna misura: run troppo corta o app morta prima del flush)")
    if summaries:
        # Window medians/p90 are summarised as their median (each window is ~2 s of frames); maxima stay maxima.
        count = sum(int(m.group(2)) for m in summaries)
        paced = [(int(m.group(6)), int(m.group(7)), int(m.group(8)), int(m.group(2))) for m in summaries]
        weight = sum(p[3] for p in paced) or 1
        vsync = [sum(p[i] * p[3] for p in paced) / weight for i in range(3)]
        med = lambda g: statistics.median(float(m.group(g)) for m in summaries)
        top = lambda g: max(float(m.group(g)) for m in summaries)
        over50 = sum(int(m.group(9)) for m in summaries)
        print(f"    frame: {count} in {len(summaries)} finestre, intervallo mediana {med(3):.1f} p90 {med(4):.1f} "
              f"max {top(5):.1f} ms, vsync 1/2/3+ {vsync[0]:.0f}/{vsync[1]:.0f}/{vsync[2]:.0f}%, oltre 50 ms {over50}")
        print(f"    pittura mediana {med(10):.1f} p90 {med(11):.1f} max {top(12):.1f}   "
              f"swap mediana {med(13):.1f} p90 {med(14):.1f} max {top(15):.1f}   "
              f"dopo pausa {sum(int(m.group(16)) for m in summaries)}   "
              f"tick dell'orologio {sum(int(m.group(17) or 0) for m in summaries)}")
        gl = [re.search(r"chiamate di disegno media ([\d.]+) max (\d+), upload ([\d.]+) per fotogramma ([\d.]+) KB, "
                        r"tempo upload mediana ([\d.]+) p90 ([\d.]+) max ([\d.]+)", m.string) for m in summaries]
        gl = [g for g in gl if g]
        if gl:
            gmed = lambda i: statistics.median(float(g.group(i)) for g in gl)
            print(f"    GL: chiamate di disegno {gmed(1):.0f}/fotogramma (max {max(int(g.group(2)) for g in gl)}), upload "
                  f"{gmed(3):.1f}/fotogramma {gmed(4):.0f} KB, tempo upload mediana {gmed(5):.1f} p90 {gmed(6):.1f} "
                  f"max {max(float(g.group(7)) for g in gl):.1f} ms")
        split = [m for m in summaries if m.group(18)]
        if split:
            smed = lambda g: statistics.median(float(m.group(g)) for m in split)
            print(f"    pittura = aggiornamento mediana {smed(18):.1f} p90 {smed(19):.1f} + disegno mediana {smed(20):.1f} "
                  f"p90 {smed(21):.1f}")
        if slow_frames and slow:
            worst = sorted(slow_frames, reverse=True)[:5]
            print("    frame lenti: " + "; ".join(
                f"#{f[3]} {f[0]:.0f} ms (pittura {f[1]:.0f}, swap {f[2]:.0f})" for f in worst))
    if frames:
        intervals = [f[0] for f in frames]
        paints = [f[1] for f in frames]
        swaps = [f[2] for f in frames]
        over50 = sum(1 for value in intervals if value > 50)
        over100 = sum(1 for value in intervals if value > 100)
        print("    " + summarize("frame", intervals))
        print(f"    {'':17} oltre 50 ms {over50:4d} ({100 * over50 / len(intervals):.1f}%)   "
              f"oltre 100 ms {over100:4d} ({100 * over100 / len(intervals):.1f}%)")
        print("    " + summarize("pittura", paints))
        print("    " + summarize("swap", swaps))
        if slow:
            worst = sorted(frames, reverse=True)[:5]
            print("    frame più lenti: " + "; ".join(
                f"#{f[3]} {f[0]:.0f} ms (pittura {f[1]:.0f}, swap {f[2]:.0f})" for f in worst))
    print("    " + summarize("tap", taps))
    if clicks:
        print("    " + summarize("tap->click", clicks))
    if gestures:
        print(f"    pan: {len(gestures)} gesti, {sum(g[0] for g in gestures)} eventi, ritardo mediana "
              f"{statistics.median(g[1] for g in gestures):.1f} p90 {statistics.median(g[2] for g in gestures):.1f} "
              f"max {max(g[3] for g in gestures):.1f} ms")
    else:
        print("    " + summarize("pan", pans))
    if pictures:
        biggest = max(pictures, key=lambda p: p[0] * p[1])
        slowest = max(pictures, key=lambda p: p[2])
        print(f"    picture del layer: {len(pictures)} registrazioni; più grande {biggest[0]}x{biggest[1]} "
              f"({biggest[2]:.1f} ms); più lenta {slowest[0]}x{slowest[1]} ({slowest[2]:.1f} ms)")
    if tasks:
        durations = [t[0] for t in tasks]
        print("    " + summarize("task lunghi", durations))
        totals = {}
        for _, _, labels in tasks:
            for name, ms in labels.items():
                totals[name] = totals.get(name, 0.0) + ms
        if totals:
            share = sum(totals.values()) or 1.0
            print("    dentro i task: " + ", ".join(f"{name} {ms:.0f} ms ({100 * ms / share:.0f}%)"
                                                   for name, ms in sorted(totals.items(), key=lambda kv: -kv[1])))
        if slow:
            worst = sorted(tasks, key=lambda t: -t[0])[:5]
            print("    task più lunghi: " + "; ".join(
                f"{t[0]:.0f} ms coda {t[1][0]}/{t[1][1]}/{t[1][2]} " +
                ",".join(f"{name} {ms:.0f}" for name, ms in sorted(t[2].items(), key=lambda kv: -kv[1])[:3])
                for t in worst))
    if windows:
        busy = [100 * w[0] / w[1] if w[1] else 0.0 for w in windows]
        ordered = sorted(busy)
        print(f"    occupato: {len(windows)} finestre da 2 s, mediana {statistics.median(ordered):.0f}% "
              f"p90 {quantile(ordered, 0.9):.0f}% max {ordered[-1]:.0f}%  "
              f"(task lunghi in totale {sum(w[4] for w in windows)})")
    for crash in crashes:
        print(f"    !! {crash}")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", default=DEFAULT_FILE)
    parser.add_argument("--runs", type=int, default=2, help="how many runs to print, newest last (0 = all)")
    parser.add_argument("--slow", action="store_true", help="list the slowest frames of each run")
    args = parser.parse_args()

    if not os.path.exists(args.file):
        raise SystemExit(f"{args.file}: non esiste")
    with open(args.file, errors="replace") as f:
        lines = f.read().splitlines()
    runs = split_runs(lines)
    print(f"{args.file}: {len(lines)} righe, {len(runs)} run\n")
    selected = runs if args.runs == 0 else runs[-args.runs:]
    offset = len(runs) - len(selected)
    for index, (_, body) in enumerate(selected):
        block(offset + index, body, args.slow)


if __name__ == "__main__":
    main()
