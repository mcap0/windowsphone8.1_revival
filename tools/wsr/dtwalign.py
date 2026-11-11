"""wsr dtwalign: phone times of a recording by DTW against MBROLA it3 saying the same sentence.

MBROLA gives exact phone times for its own audio (espeak --pho); dynamic time warping on MFCC (c1-c12 with the
utterance mean removed, so speaker and microphone tilt mostly cancel) maps every frame of the recording to a frame of
the synthetic one, and the phone boundaries follow the path. No model, no training: a template match, used only to
label recordings for measuring and for the Lumia fit.

    python3 tools/wsr/dtwalign.py file.wav "testo"
"""
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear  # noqa: E402
import vowels as V  # noqa: E402


def feats(x):
    e = ear.Ear(x)
    c = e.mfcc[:, 1:13].copy()
    sp = e.energy > e.floor + 15          # mean over speech frames only
    c -= c[sp].mean(0) if sp.any() else c.mean(0)
    c /= c[sp].std(0) + 1e-6 if sp.any() else 1.0
    return e, c


def dtw(A, B, band=0.25):
    """Path (i in A, j in B) minimising the summed Euclidean distance; steps (1,0) (0,1) (1,1); a Sakoe-Chiba band."""
    n, m = len(A), len(B)
    D = np.sqrt(((A[:, None, :] - B[None, :, :]) ** 2).sum(2))
    INF = 1e18
    C = np.full((n + 1, m + 1), INF)
    C[0, 0] = 0
    w = max(int(band * max(n, m)), abs(n - m) + 5)
    for i in range(1, n + 1):
        jc = int(i * m / n)
        lo, hi = max(1, jc - w), min(m, jc + w)
        for j in range(lo, hi + 1):
            C[i, j] = D[i - 1, j - 1] + min(C[i - 1, j], C[i, j - 1], C[i - 1, j - 1])
    i, j, path = n, m, []
    while i > 0 and j > 0:
        path.append((i - 1, j - 1))
        k = np.argmin([C[i - 1, j - 1], C[i - 1, j], C[i, j - 1]])
        i, j = (i - 1, j - 1) if k == 0 else (i - 1, j) if k == 1 else (i, j - 1)
    return path[::-1]


def speech_span(e):
    """Frames from the first to the last frame clearly above the floor."""
    sp = np.where(e.energy > e.floor + 15)[0]
    return (int(sp[0]), int(sp[-1]) + 1) if len(sp) else (0, e.T)


def align(x, text, ref_voice='mb-it3', speed=None):
    """[(mbrola phone, start frame, end frame)] on the recording x. The reference is made at the recording's own
    speaking rate: one synthesis at 150 wpm, then again at 150 * (its speech length / the recording's)."""
    ex, A = feats(x)
    a0, a1 = speech_span(ex)
    with tempfile.TemporaryDirectory() as d:
        got = V.synth(ref_voice, text, 150, 1.0, d)
        if got is None:
            return None
        if speed is None:
            ey0 = ear.Ear(got[0])
            b0, b1 = speech_span(ey0)
            speed = int(np.clip(150 * (b1 - b0) / max(a1 - a0, 1), 80, 320))
            got = V.synth(ref_voice, text, speed, 1.0, d)
            if got is None:
                return None
    y, segs = got
    ey, B = feats(y)
    # silence before and after the speech is cut from both, so the path's ends are the speech's ends (the phone
    # recordings start with ~0.7 s of silence, MBROLA with almost none: DTW from (0,0) smeared the first phones
    # over it)
    a0, a1 = speech_span(ex)
    b0, b1 = speech_span(ey)
    a0, b0 = max(0, a0 - 3), max(0, b0 - 3)
    a1, b1 = min(len(A), a1 + 3), min(len(B), b1 + 3)
    path = [(i + a0, j + b0) for i, j in dtw(A[a0:a1], B[b0:b1])]
    # for each synthetic frame, the recording frames mapped to it
    first, last = {}, {}
    for i, j in path:
        first.setdefault(j, i)
        last[j] = i
    out = []
    for ph, a, b in segs:
        ja = int(a / 10)
        jb = max(ja + 1, int(b / 10))
        ja = min(max(ja, b0), b1 - 1)
        jb = min(max(jb, ja + 1), b1)
        ia = first.get(ja, a0)
        ib = last.get(jb - 1, ia) + 1
        out.append((ph, ia, ib))
    return out, ex


if __name__ == '__main__':
    r, e = align(ear.load(sys.argv[1]), sys.argv[2])
    for ph, a, b in r:
        print(f'{a * 10:6d}-{b * 10:6d} ms  {ph}')
