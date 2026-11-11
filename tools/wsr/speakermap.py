"""wsr speakermap: an affine map from MBROLA it3's MFCC to speaker's (speaker-dependent recognizer, 2026-11-03).

Pairs of frames come from the DTW paths of the fit takes (fit.dataset('fit')): recording frame <-> MBROLA frame
saying the same thing. Least squares gives W (12 x 13: 12 cepstra + bias) with speaker ~= [mbrola, 1] @ W.T,
written to tools/wsr/voce-speaker.txt as plain numbers (readable and hand-editable; the diagonal is how much each
cepstrum of MBROLA survives in speaker's voice, the off-diagonal terms how the two voices' spectra differ).
The decoder applies it to its templates.

    python3 tools/wsr/speakermap.py          fit -> voce-speaker.txt
"""
import os
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtwalign  # noqa: E402
import ear  # noqa: E402
import fit  # noqa: E402
import vowels as V  # noqa: E402

OUT = os.path.join(HERE, 'voce-speaker.txt')
RIDGE = 50.0     # shrink towards the identity (hand choice: thousands of noisy pairs, 156 numbers)


def pairs(x, text):
    ex, A = dtwalign.feats(x)
    a0, a1 = dtwalign.speech_span(ex)
    with tempfile.TemporaryDirectory() as d:
        got = V.synth('mb-it3', ' '.join(text.lower().split()), 150, 1.0, d)
        if got is None:
            return None
        b0, b1 = dtwalign.speech_span(ear.Ear(got[0]))
        speed = int(np.clip(150 * (b1 - b0) / max(a1 - a0, 1), 80, 320))
        got = V.synth('mb-it3', ' '.join(text.lower().split()), speed, 1.0, d)
        if got is None:
            return None
    ey, B = dtwalign.feats(got[0])
    b0, b1 = dtwalign.speech_span(ey)
    a0, b0 = max(0, a0 - 3), max(0, b0 - 3)
    a1, b1 = min(len(A), a1 + 3), min(len(B), b1 + 3)
    path = dtwalign.dtw(A[a0:a1], B[b0:b1])
    return np.array([A[i + a0] for i, j in path]), np.array([B[j + b0] for i, j in path])


def load(path=OUT):
    W = []
    for line in open(path, encoding='utf-8'):
        p = line.split('#')[0].split()
        if p and p[0].startswith('c'):
            W.append([float(x) for x in p[1:]])
    return np.array(W)


def apply(B, W):
    return np.hstack([B, np.ones((len(B), 1))]) @ W.T


def main():
    Xs, Ys = [], []
    for path, text, k in fit.dataset('fit'):
        r = pairs(ear.load(path), text)
        if r is None:
            continue
        Xs.append(r[1])
        Ys.append(r[0])
    X = np.vstack(Xs)
    Y = np.vstack(Ys)
    Xa = np.hstack([X, np.ones((len(X), 1))])
    I = np.hstack([np.eye(12), np.zeros((12, 1))])
    # ridge towards the identity map: (Xa'Xa + R) W' = Xa'Y + R I'
    R = RIDGE * np.eye(13)
    W = np.linalg.solve(Xa.T @ Xa + R, Xa.T @ Y + R @ I.T).T
    lines = ['# speaker ~= W @ [mbrola c1..c12, 1]   (rows: speaker c1..c12; columns: mbrola c1..c12, bias)',
             f'# fit on {len(X)} frame pairs from {len(Xs)} takes (fit.dataset("fit")), ridge {RIDGE} towards identity',
             '# hand edits go below the row they change']
    for i in range(12):
        lines.append(f'c{i + 1:<3d}' + ' '.join(f'{v:6.2f}' for v in W[i]))
    open(OUT, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    res0 = np.sqrt(((Y - X) ** 2).sum(1)).mean()
    res1 = np.sqrt(((Y - apply(X, W)) ** 2).sum(1)).mean()
    print(f'{len(X)} pairs; mean frame distance before {res0:.3f}, after {res1:.3f} -> {OUT}')


if __name__ == '__main__':
    main()
