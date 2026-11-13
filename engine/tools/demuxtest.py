#!/usr/bin/env python3
"""demuxtest.py [file ...] — compares what engine/port/media/Demux.cpp reads with ffprobe's packets.

Builds engine/port/tests/demuxtest.cpp for the PC, runs it on each file (default: share/browser/media and the
variants ffmpeg writes from it, see variants()) and checks,
track by track in decode order, every sample's size, key flag and time (1 ms; MP3 and ADTS: sizes only, ffprobe shifts
MP3 times by the encoder delay and their seeks go by byte position). Then seeks: the first sample of each track after a seek must be a packet ffprobe has, a key
frame at or before the time for video, the frame holding the time for audio."""
import os, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..")
SOURCES = [os.path.join(ROOT, "engine", "port", "tests", "demuxtest.cpp"), os.path.join(ROOT, "engine", "port", "media", "Demux.cpp"),
           os.path.join(ROOT, "engine", "port", "media", "Hls.cpp")]


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
    hls = ["-allowed_extensions", "ALL", "-protocol_whitelist", "file,crypto,data"] if path.endswith(".m3u8") else []
    text = subprocess.run(["ffprobe", "-v", "error"] + hls + ["-show_entries", "packet=stream_index,pts_time,size,flags,duration_time",
                           "-of", "csv=p=0", path], capture_output=True, text=True).stdout
    kinds = subprocess.run(["ffprobe", "-v", "error"] + hls + ["-show_entries", "stream=index,codec_type", "-of", "csv=p=0", path],
                           capture_output=True, text=True).stdout
    kind = {int(l.split(",")[0]): l.split(",")[1] for l in kinds.splitlines() if l}
    tracks = {}
    for line in text.splitlines():
        f = line.split(",")
        if len(f) < 5 or f[1] == "N/A":
            continue
        duration = 0 if f[2] == "N/A" else round(float(f[2]) * 1e6)
        tracks.setdefault(int(f[0]), []).append((round(float(f[1]) * 1e6), int(f[3]), "K" in f[4], duration))
    if tracks:
        # Ours start where the earliest track does (transport streams start anywhere; an MP4 whose first time is not
        # 0 is moved to 0, Demux.cpp's ShiftedDemuxer). Times that start at 0 or before stay as they are.
        first = min(min(p[0] for p in t) for t in tracks.values())
        if path.endswith((".m3u8", ".ts")) or first > 0:
            tracks = {k: [(p[0] - first, p[1], p[2], p[3]) for p in t] for k, t in tracks.items()}
    return tracks, kind


def duration(path):
    """ffprobe's duration of the container (seconds from its start), microseconds; None for a stream without one."""
    text = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                          capture_output=True, text=True).stdout.strip()
    try:
        return round(float(text) * 1e6)
    except ValueError:
        return None


def variants():
    """Files ffmpeg makes from h264-aac.mp4 for layouts the fixed set lacks: a presentation that starts at 31.4 s
    (-output_ts_offset 30 plus the transport stream muxer's 1.4 s) as a plain file (an empty edit), as fragments with
    an mfra and with a sidx; ffmpeg's default fragmented layout (the first fragment's samples in moov, the rest in
    fragments), with and without the offset; an mfra whose times are 30 s off the fragments' own (-output_ts_offset on
    a fragmented file moves the index, not the tfdt); fragments of one track each (separate_moof: the other track's
    fragment for a time is further on), with an mfra and the offset, and without any index; interleaved fragments
    without an index (skip_trailer: no mfra)."""
    src = os.path.join(ROOT, "share", "browser", "media", "h264-aac.mp4")
    out = os.path.join(tempfile.gettempdir(), "lumia-demuxtest-media")
    os.makedirs(out, exist_ok=True)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ts = os.path.join(out, "at30.ts")
    subprocess.check_call(ff + ["-i", src, "-c", "copy", "-output_ts_offset", "30", ts])
    at30 = ["-copyts", "-i", ts, "-c", "copy", "-bsf:a", "aac_adtstoasc"]
    frag = "frag_keyframe+empty_moov+default_base_moof"
    recipes = [("prog-at30.mp4", at30),
               ("frag-at30.mp4", at30 + ["-movflags", frag]),
               ("sidx-at30.mp4", at30 + ["-movflags", frag + "+global_sidx"]),
               ("hybrid-at30.mp4", at30 + ["-movflags", "frag_keyframe"]),
               ("hybrid.mp4", ["-i", src, "-c", "copy", "-movflags", "frag_keyframe"]),
               ("badmfra.mp4", ["-i", src, "-c", "copy", "-movflags", frag, "-output_ts_offset", "30"]),
               ("sep-at30.mp4", at30 + ["-movflags", frag + "+separate_moof"]),
               ("sep-noindex.mp4", ["-i", src, "-c", "copy", "-movflags", frag + "+separate_moof+skip_trailer"]),
               ("frag-noindex.mp4", ["-i", src, "-c", "copy", "-movflags", frag + "+skip_trailer"])]
    files = []
    for name, args in recipes:
        files.append(os.path.join(out, name))
        subprocess.check_call(ff + args + [files[-1]])
    return files


def mse(binary, path):
    """The stream parser fed in pieces must give what the demuxer reads from the whole file."""
    whole = subprocess.run([binary, path], capture_output=True, text=True).stdout
    whole = sorted(l for l in whole.splitlines() if not l.startswith("#"))
    bad = 0
    for piece in (1, 777, 65536, 1 << 30):
        text = subprocess.run([binary, "--mse", path, str(piece)], capture_output=True, text=True).stdout
        got = sorted(l for l in text.splitlines() if not l.startswith("#"))
        ok = got == whole and bool(got)
        print("%-14s mse a pezzi di %d: %d campioni%s" % (os.path.basename(path), piece, len(got), "" if ok else "  ERRORE"))
        bad += not ok
    return bad


def main():
    files = sys.argv[1:] or [os.path.join(ROOT, "share", "browser", "media", n) for n in ("h264-aac.mp4", "frag.mp4", "tone.m4a", "tone.mp3", "hls.m3u8", "hls0.ts")] + variants()
    binary = build()
    bad = 0
    if files and files[0] == "--mse":
        return 1 if sum(mse(binary, f) for f in files[1:]) else 0
    if not sys.argv[1:]:
        bad += mse(binary, os.path.join(ROOT, "share", "browser", "media", "frag.mp4"))
    for path in files:
        name = os.path.basename(path)
        mine, notes = ours(binary, path)
        ref, kind = theirs(path)
        if not mine:
            print("%s: %s" % (name, "; ".join(notes)))
            bad += 1
            continue
        # The duration we give the element: within 50 ms of the container's (the last sample's end for a fragmented
        # file without a declared one). Fragments with no mehd, mfra or sidx have none we can know without reading
        # every one of them (range requests on the phone): unknown, 0 (Chrome says Infinity).
        declared = [int(n.split("durata ")[1].split(" ")[0]) for n in notes if n.startswith("# ") and "durata" in n and "traccia" not in n]
        expected = duration(path)
        if name.endswith("noindex.mp4") and declared == [0]:
            expected = None
        if declared and expected and not name.endswith((".m3u8", ".ts")) and abs(declared[0] - expected) > 50000:
            print("%-14s durata %d us, ffprobe %d us  ERRORE" % (name, declared[0], expected))
            bad += 1
        mp3 = name.endswith(".mp3") or name.endswith(".aac")
        header = 7 if name.endswith(".aac") or any("(ts)" in n or "mpeg-ts" in n for n in notes) else 0  # ffprobe counts the ADTS header, the decoder does not want it
        fragmented = any("frammenti" in n for n in notes)
        # HLS enters at a segment's start: a time in its first instants, before the segment's first key frame or
        # sound frame, starts there and not in the segment before.
        hls = any("# hls" in n for n in notes)
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
                if x[1] + (0 if video else header) != y[1] or (video and x[2] != y[2]) or (not mp3 and abs(x[0] - y[0]) > 1000):
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
                    ok = ok or (hls and first[2] and 0 <= first[0] - at < 100000)
                else:
                    hold = [p for p in b if p[0] <= at + 1000 < p[0] + p[3] + 1000]
                    ok = any(abs(first[0] - p[0]) <= 1000 for p in hold)
                    # A fragment found through the video's index starts its audio a frame or two after the video.
                    ok = ok or ((fragmented or hls) and 0 <= first[0] - at < 50000)
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
