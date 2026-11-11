"""wsr fit: the vowel table measured on the Lumia (allowed by the user, 2026-11-03; see MISURE.md).

Takes: share/voce/*-testo<speaker>/ (the agreed reading). Each take is labelled by dtwalign.py (MBROLA it3 saying
the same line, espeak's phonemes: e1/E1 o1/O1 tell closed/open where espeak's dictionary knows; unstressed e o
are closed). Vowels of at least 5 frames, middle 50%, voiced frames with F1 F2. Per vowel: median and
1.4826*MAD in Bark of the raw F1 F2 (speaker is the reference speaker: no warp), widths at least 0.25 Bark.
Odd takes fit, even takes test (same speaker, different sentences); other speakers are tested with --test.

    python3 tools/wsr/fit.py speaker            fit on odd takes -> tools/wsr/lumia.json, report on even takes
    python3 tools/wsr/fit.py --test mamma      measure only (all takes), with the current table
"""
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtwalign  # noqa: E402
import ear  # noqa: E402

VOICE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'share', 'voce')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lumia.json')
LAB = {'a1': 'a', 'a': 'a', 'i1': 'i', 'i': 'i', 'u1': 'u', 'u': 'u', 'e1': 'e', 'e': 'e', 'E1': 'ɛ',
       'o1': 'o', 'o': 'o', 'O1': 'ɔ'}


def takes(speaker):
    out = []
    pattern = f'*-{speaker[1:]}' if speaker.startswith('=') else f'*-testo{speaker}'   # '=lungopodcast-frasi'
    for d in sorted(glob.glob(os.path.join(VOICE, pattern))):
        for line in open(os.path.join(d, 'elenco.tsv'), encoding='utf-8'):
            p = line.rstrip('\n').split('\t')
            if len(p) < 2:
                continue
            out.append((os.path.join(d, p[0]), p[1], int(p[0][:3])))
    return out


def dataset(split):
    """speaker's recordings for the speaker-dependent recognizer (user's choice, 2026-11-03).
    'test' = the even command takes (never used to fit or tune anything); 'fit' = everything else that has a
    transcript: the reading, the odd command takes, and takes 8-50 of the first session (syllables, words,
    commands; 1-7 are single held vowels)."""
    out = []
    for d in sorted(glob.glob(os.path.join(VOICE, '*'))):
        name = os.path.basename(d)
        kind = ('comandi' if '-comandimatte' in name else 'testo' if name.endswith('-testospeaker')
                else 'prompt' if name.endswith('-speaker') else None)
        if kind is None or not os.path.exists(os.path.join(d, 'elenco.tsv')):
            continue
        for line in open(os.path.join(d, 'elenco.tsv'), encoding='utf-8'):
            p = line.rstrip('\n').split('\t')
            if len(p) < 2:
                continue
            k = int(p[0][:3])
            if kind == 'prompt' and k < 8:
                continue
            test = kind == 'comandi' and k % 2 == 0
            if (split == 'test') == test:
                out.append((os.path.join(d, p[0]), p[1], k))
    return out


def labelled(path, text):
    """[(label, frame indices)] for the vowels of one take, and the Ear."""
    r = dtwalign.align(ear.load(path), text)
    if r is None:
        return [], None
    segs, e = r
    out = []
    for ph, a, b in segs:
        v = LAB.get(ph)
        if v is None or b - a < 5:
            continue
        lo, hi = a + (b - a) // 4, b - (b - a) // 4
        out.append((v, list(range(lo, max(hi, lo + 1)))))
    return out, e


def collect(tk):
    data = {}
    for path, text, k in tk:
        vs, e = labelled(path, text)
        for v, fr in vs:
            for t in fr:
                if e.voiced[t] and np.isfinite(e.F[t, 0]) and np.isfinite(e.F[t, 1]):
                    data.setdefault(v, []).append(e.F[t, :2])
    return data


def measure(tk):
    import vowels as V
    rows = []
    for path, text, k in tk:
        vs, e = labelled(path, text)
        if e is None:
            continue
        sc = V.frame_scores(e)
        for v, fr in vs:
            for t in fr:
                g = V.VOWELS[int(sc[t].argmax())] if np.isfinite(sc[t]).any() else None
                rows.append(('testo', v, g))
    return rows


def main():
    if sys.argv[1] == '--test':
        import vowels as V
        rows = measure(takes(sys.argv[2]))
        print(f'table {V.TABLE}, speaker {sys.argv[2]}, all takes')
        print(V.report(rows, {'testo': [0]}))
        return
    spk = sys.argv[1]
    tk = takes(spk)
    fit_tk = [t for t in tk if t[2] % 2 == 1]
    test_tk = [t for t in tk if t[2] % 2 == 0]
    data = collect(fit_tk)
    import vowels as V0
    table = {}
    for v in V0.VOWELS:
        X = np.array(data.get(v, []))
        if len(X) < 20:
            f1, f2 = V0.REF[v] if V0.TABLE != 'lumia' else (None, None)
            table[v] = [float(V0.REF[v][0]), float(V0.REF[v][1]), 0.55, 0.85, int(len(X))]
            continue
        Z = ear.bark(X)
        med = np.median(Z, 0)
        mad = 1.4826 * np.median(np.abs(Z - med), 0)
        sd = np.maximum(mad, 0.25)
        hz = 1960.0 * (med + 0.53) / (26.28 - med)
        table[v] = [float(round(hz[0])), float(round(hz[1])), float(round(sd[0], 3)), float(round(sd[1], 3)), int(len(X))]
    json.dump(table, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('fit on', len(fit_tk), 'takes ->', OUT)
    for v in V0.VOWELS:
        print(f'  {v}: F1 {table[v][0]:5.0f} F2 {table[v][1]:5.0f}  sd {table[v][2]:.2f} {table[v][3]:.2f} Bark  n {table[v][4]}')
    print('test on', len(test_tk), 'even takes: run  python3 tools/wsr/fit.py --heldout', spk)


if __name__ == '__main__':
    if sys.argv[1] == '--heldout':
        import vowels as V
        tk = [t for t in takes(sys.argv[2]) if t[2] % 2 == 0]
        rows = measure(tk)
        print(f'table {V.TABLE}, speaker {sys.argv[2]}, even takes')
        print(V.report(rows, {'testo': [0]}))
    else:
        main()
