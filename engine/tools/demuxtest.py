#!/usr/bin/env python3
"""demuxtest.py [file ...] — compares what engine/port/media/Demux.cpp reads with ffprobe's packets.

Builds engine/port/tests/demuxtest.cpp for the PC, runs it on each file (default: share/browser/media) and checks,
track by track in decode order, every sample's size, key flag and time (1 ms; MP3 and ADTS: sizes only, ffprobe shifts
MP3 times by the encoder delay and their seeks go by byte position). Then seeks: the first sample of each track after a seek must be a packet ffprobe has, a key
frame at or before the time for video, the frame holding the time for audio."""
import os, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..")
SOURCES = [os.path.join(ROOT, "engine", "port", "tests", "demuxtest.cpp"), os.path.join(ROOT, "engine", "port", "media", "Demux.cpp")]


def build():
    out = os.path.join(tempfile.gettempdir(), "lumia-demuxtest")
    subprocess.check_call(["clang++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-o", out] + SOURCES)
    return out


def ours(binary, path, seek=None):
    text = subprocess.run([binary, path] + ([str(seek)] if seek is not None else []), capture_output=True, text=True).stdout
    tracks, notes = {}, []
    for line in text.splitlines():
        if line.startswith("#") or line.startswith("errore"):
            notes.append(line)
            continue
        t, pts, size, key = line.split()
        tracks.setdefault(int(t), []).append((int(pts), int(size), key == "K"))
    return tracks, notes


def theirs(path):
    text = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "packet=stream_index,pts_time,size,flags,duration_time",
                           "-of", "csv=p=0", path], capture_output=True, text=True).stdout
    kinds = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=index,codec_type", "-of", "csv=p=0", path],
                           capture_output=True, text=True).stdout
    kind = {int(l.split(",")[0]): l.split(",")[1] for l in kinds.splitlines() if l}
    tracks = {}
    for line in text.splitlines():
        f = line.split(",")
        if len(f) < 5 or f[1] == "N/A":
            continue
        duration = 0 if f[2] == "N/A" else round(float(f[2]) * 1e6)
        tracks.setdefault(int(f[0]), []).append((round(float(f[1]) * 1e6), int(f[3]), "K" in f[4], duration))
    return tracks, kind


def main():
    files = sys.argv[1:] or [os.path.join(ROOT, "share", "browser", "media", n) for n in ("h264-aac.mp4", "frag.mp4", "tone.m4a", "tone.mp3")]
    binary = build()
    bad = 0
    for path in files:
        name = os.path.basename(path)
        mine, notes = ours(binary, path)
        ref, kind = theirs(path)
        if not mine:
            print("%s: %s" % (name, "; ".join(notes)))
            bad += 1
            continue
        mp3 = name.endswith(".mp3") or name.endswith(".aac")
        header = 7 if name.endswith(".aac") else 0  # ffprobe counts the ADTS header, the decoder does not want it
        fragmented = any("frammenti" in n for n in notes)
        # Our tracks are in the file's order of the kinds we keep; pair by kind.
        order = sorted(ref)
        pairs = []
        for t in sorted(mine):
            video = any("video" in n and ("traccia %d:" % t) in n for n in notes)
            match = [s for s in order if kind.get(s) == ("video" if video else "audio")]
            pairs.append((t, match[0] if match else None, video))
        for t, s, video in pairs:
            a, b = mine[t], ref.get(s, [])
            errors = []
            if len(a) != len(b):
                errors.append("campioni %d, ffprobe %d" % (len(a), len(b)))
            for i, (x, y) in enumerate(zip(a, b)):
                if x[1] + header != y[1] or (video and x[2] != y[2]) or (not mp3 and abs(x[0] - y[0]) > 1000):
                    errors.append("campione %d: nostro %s, ffprobe %s" % (i, x, y[:3]))
                    break
            # Seeks.
            last = max(p[0] + p[3] for p in b) if b else 0
            seeks = 0
            for k in range(1, 8):
                at = last * k // 8
                got, _ = ours(binary, path, at / 1e6)
                first = got.get(t, [None])[0]
                if first is None:
                    errors.append("seek %.2f: niente" % (at / 1e6))
                    continue
                if mp3:
                    ok = abs(first[0] - at) < 1000
                elif video:
                    keys = [p for p in b if p[2] and p[0] <= at + 1000]
                    ok = bool(keys) and abs(first[0] - max(keys)[0]) <= 1000 and first[2]
                else:
                    hold = [p for p in b if p[0] <= at + 1000 < p[0] + p[3] + 1000]
                    ok = any(abs(first[0] - p[0]) <= 1000 for p in hold)
                    # A fragment found through the video's index starts its audio a frame or two after the video.
                    ok = ok or (fragmented and 0 <= first[0] - at < 50000)
                if ok:
                    seeks += 1
                else:
                    errors.append("seek %.2f: primo campione %s" % (at / 1e6, first))
            print("%-14s %s %d campioni, %d/7 seek%s" % (name, "video" if video else "audio", len(a), seeks,
                                                        "" if not errors else "  ERRORE " + "; ".join(errors[:3])))
            bad += bool(errors)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
