"""Vowel test on real recordings from Assistente's recording mode (share/voce/<session>/).

Labels without any alignment model: takes 001-007 are one held vowel each (a è é i ò ó u); takes 008-019 are
five syllables (C+a e i o u). Only the first voiced run of a held take counts (the rest may be the prompt read aloud); the voiced runs of a take (at least 6 frames = 60 ms) are its vowels, in order; if
the count does not match the prompt, the take is reported and skipped. The middle 50% of each run is scored,
as in the synthetic sets. Syllable e/o are not known open or closed: they count only in the 5-way score.

    python3 tools/wsr/realtest.py [session ...]      (default: every session in share/voce)
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear  # noqa: E402
import vowels as V  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'share', 'voce')
HELD = {1: 'a', 2: 'ɛ', 3: 'e', 4: 'i', 5: 'ɔ', 6: 'o', 7: 'u'}
SYL = ['a', '(e)', 'i', '(o)', 'u']


def runs(mask, minlen=6):
    out, t = [], 0
    while t < len(mask):
        if mask[t]:
            s = t
            while t < len(mask) and mask[t]:
                t += 1
            if t - s >= minlen:
                out.append((s, t))
        else:
            t += 1
    return out


def take_rows(path, expect, tag):
    e = ear.Ear(ear.load(path))
    sc = V.frame_scores(e)
    rs = runs(e.voiced)
    if len(expect) == 1:
        # a held vowel: only the first voiced run (session speaker read the whole prompt aloud,
        # "è aperta, come in bène": what follows the vowel is other words)
        rs = rs[:1]
    elif rs and len(rs) % len(expect) == 0:  # the syllables said twice or more
        expect = expect * (len(rs) // len(expect))
    if not rs or len(rs) != len(expect):
        return None, e, len(rs)
    rows = []
    for (s, t), lab in zip(rs, expect):
        a, b = s + (t - s) // 4, t - (t - s) // 4
        for u in range(a, max(b, a + 1)):
            g = V.VOWELS[int(sc[u].argmax())] if np.isfinite(sc[u]).any() else None
            rows.append((tag, lab, g))
    return rows, e, len(rs)


def main():
    sessions = sys.argv[1:] or sorted(os.listdir(ROOT))
    for sess in sessions:
        d = os.path.join(ROOT, sess)
        rows, Ls, skipped = [], {}, []
        for k in range(1, 20):
            path = os.path.join(d, f'{k:03d}.wav')
            if not os.path.exists(path):
                continue
            expect = [HELD[k]] if k in HELD else SYL
            tag = 'vocali tenute' if k in HELD else 'sillabe'
            r, e, n = take_rows(path, expect, tag)
            Ls.setdefault(tag, []).append(e.Lw if hasattr(e, 'Lw') else e.L)
            if r is None:
                skipped.append(f'{k:03d} ({n} tratti sonori, attesi {len(expect)})')
                continue
            rows += r
        print(f'== {sess}')
        if skipped:
            print('saltate:', ', '.join(skipped))
        print(V.report(rows, Ls))
        held = [r for r in rows if r[0] == 'vocali tenute']
        for v in ['a', 'ɛ', 'e', 'i', 'ɔ', 'o', 'u']:
            sel = [r for r in held if r[1] == v]
            if sel:
                cnt = {}
                for r in sel:
                    cnt[r[2]] = cnt.get(r[2], 0) + 1
                print(f'  tenuta {v}: ' + ' '.join(f'{g}:{c}' for g, c in sorted(cnt.items(), key=lambda x: -x[1])))


if __name__ == '__main__':
    main()
