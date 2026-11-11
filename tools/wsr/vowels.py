"""wsr vowels: the 7 Italian vowels as written regions of the normalised F1-F2 plane (VOCE-PIANO.md 4.2).

The centres are adult-male Italian means from the literature (Ferrero 1972, as tabulated in Albano Leoni & Maturi,
"Manuale di fonetica"), rounded; the plan's own figures (/a/ 750-1300, /i/ 300-2300, /u/ 320-750) are among them.
The formants of every speaker are first scaled to the 17.5 cm reference tract by ear.py (f' = f L / 17.5), so one
table serves men, women and children. Nothing here is computed from recordings.

The score of vowel v in a frame is a Gaussian log-likelihood in Bark:
    -0.5 * ((z1 - Z1v) / S1)^2 - 0.5 * ((z2 - Z2v) / S2)^2
S1 = 0.55 Bark and S2 = 0.85 Bark: the spread of one vowel category across Italian speakers is about half a Bark
on F1 and nearly a Bark on F2 (F2 depends more on lip rounding and coarticulation). The closed/open pairs e/ɛ and
o/ɔ are ~1.2 Bark apart on F1, so they overlap; the decoder will get both scores (soft, never a hard decision), and
the regional variation of e/ɛ, o/ɔ in Italian makes a hard choice wrong anyway.

    python3 tools/wsr/vowels.py --test [--set NAME]    syllables from espeak-ng/MBROLA, labels from --pho
"""
import argparse
import datetime
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear  # noqa: E402

VOWELS = ['i', 'e', 'ɛ', 'a', 'ɔ', 'o', 'u']
# adult male Italian, Hz (F1, F2)
REF = {
    'i': (290, 2250),
    'e': (380, 2000),
    'ɛ': (540, 1830),
    'a': (750, 1300),
    'ɔ': (570, 980),
    'o': (410, 830),
    'u': (320, 750),
}
S1, S2 = 0.55, 0.85
# The Lumia table (tools/wsr/fit.py, allowed by the user on 2026-11-03: "adatti tutto al microfono del lumia"):
# centres and widths measured on session testospeaker through the Lumia microphone; it replaces the literature
# table unless WSR_TABLE=ferrero. Its reference speaker is speaker (warp 17.5 cm = speaker).
TABLE = os.environ.get('WSR_TABLE', 'ferrero')  # the fitted formant table was worse on held-out sentences (MISURE.md)
_LUMIA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lumia.json')
SW = np.array([[S1, S2]] * len(VOWELS))
if TABLE == 'lumia' and os.path.exists(_LUMIA):
    import json
    _t = json.load(open(_LUMIA, encoding='utf-8'))
    REF = {v: (_t[v][0], _t[v][1]) for v in VOWELS}
    SW = np.array([[_t[v][2], _t[v][3]] for v in VOWELS])
Z = np.array([[ear.bark(f1), ear.bark(f2)] for f1, f2 in (REF[v] for v in VOWELS)])
# five-way collapse (the decoder's G2P will mostly not know e/ɛ, o/ɔ either)
FIVE = {'i': 'i', 'e': 'e', 'ɛ': 'e', 'a': 'a', 'ɔ': 'o', 'o': 'o', 'u': 'u'}


def scores(Fn):
    """Frames x 7 log-scores from normalised formants (NaN rows -> -inf: no formants, no vowel)."""
    z1 = ear.bark(Fn[:, 0])[:, None]
    z2 = ear.bark(Fn[:, 1])[:, None]
    s = (-0.5 * ((z1 - Z[None, :, 0]) / SW[None, :, 0]) ** 2 - 0.5 * ((z2 - Z[None, :, 1]) / SW[None, :, 1]) ** 2
         - np.log(SW[:, 0] * SW[:, 1])[None, :])
    return np.where(np.isfinite(s), s, -np.inf)


# Speaker warp (VTLN by maximum likelihood, Lee & Rose 1996), computed per utterance from the utterance only.
# Candidate tract lengths 11..20 cm. For each, the formants are scaled to the reference and every frame scores its
# best-fitting vowel (soft: log-sum-exp); the total is added to two written terms:
#   - an anthropometric prior: adult tract 14.5 (women) .. 17.5 cm (men), so N(16, 1.5 cm);
#   - the formant-dispersion estimate from ear.py, N(L_disp, 1.5 cm) (one utterance's dispersion is noisy).
# Frame scores are divided by the number of frames, times 8: one utterance counts like ~8 independent looks,
# not one per 10 ms (neighbouring frames see the same vowel).
L_GRID = np.arange(11.0, 20.01, 0.25)
L_PRIOR, L_PRIOR_SD, L_DISP_SD = 16.0, 1.5, 1.5


def warp(e):
    """Tract length (cm) for the utterance in Ear e."""
    ok = np.isfinite(e.F[:, 0]) & np.isfinite(e.F[:, 1])
    if ok.sum() == 0:
        return e.L
    best, bestL = -np.inf, e.L
    for L in L_GRID:
        sc = scores(e.F[ok] * (L / ear.L_REF))
        m = sc.max(1, keepdims=True)
        fr = (m[:, 0] + np.log(np.exp(sc - m).sum(1)))
        tot = 8.0 * fr.mean()
        tot += -0.5 * ((L - L_PRIOR) / L_PRIOR_SD) ** 2 - 0.5 * ((L - e.L) / L_DISP_SD) ** 2
        if tot > best:
            best, bestL = tot, L
    return float(bestL)


def normalised(e):
    """Formants of Ear e scaled to the reference speaker with the warp; also sets e.Lw."""
    e.Lw = warp(e)
    return e.F * (e.Lw / ear.L_REF)


# ----------------------------------------------------------------------------- analysis by synthesis (v2)
# No formant picking. For each vowel and each candidate tract length the spectrum the tube would make is written
# from acoustic theory (Fant 1960: a cascade of resonators, |H(f)|^2 = prod (Fk^2 + Bk^2/4)^2 /
# [((f - Fk)^2 + Bk^2/4)((f + Fk)^2 + Bk^2/4)]), smoothed exactly like the observed spectrum (same cepstral
# lifter), and compared with it on a Bark grid 150-4000 Hz after removing the mean and the linear tilt of both
# (the tilt carries the voice source, the microphone and the room, not the vowel). Bell et al. 1961 called it
# analysis by synthesis. Merged F1-F2 of /u o/ and weak F3 are just spectra here: nothing to track.
# F3: higher in front vowels (2900-3000 Hz for male /i/), near the neutral 2500 elsewhere; F4, F5 neutral tube
# (3500, 4500). Bandwidths: Fant's typical 60 / 80 / 120 / 180 / 250 Hz.
F3REF = {'i': 2950, 'e': 2600, 'ɛ': 2550, 'a': 2500, 'ɔ': 2450, 'o': 2400, 'u': 2350}
BW = np.array([60.0, 80.0, 120.0, 180.0, 250.0])
ZGRID = np.linspace(ear.bark(150.0), ear.bark(4000.0), 32)
FGRID = 1960.0 * (ZGRID + 0.53) / (26.28 - ZGRID)      # inverse of Traunmueller's formula
SIGMA_DB = 5.0     # spread of a vowel's smoothed spectrum around the tube's, per grid point
K_EFF = 8.0        # the 32 grid points are not independent: about 8 independent spectral looks


def _bins_of(f):
    return np.clip(np.round(f / (ear.SR / ear.NFFT)).astype(int), 0, ear.NFFT // 2)


def _smooth_log(logP, cut):
    """Cepstral smoothing: keep quefrencies below `cut` samples (rows = spectra on the 257 FFT bins)."""
    c = np.fft.irfft(logP, ear.NFFT)
    c[:, cut:ear.NFFT - cut + 1] = 0.0
    return np.fft.rfft(c, ear.NFFT).real[:, :ear.NFFT // 2 + 1]


def _flat(Y):
    """Remove mean and linear slope over the Bark grid, per row."""
    z = ZGRID - ZGRID.mean()
    Y = Y - Y.mean(1, keepdims=True)
    return Y - ((Y * z).sum(1, keepdims=True) / (z * z).sum()) * z


def tube_logspec(F):
    """log power (natural log) of the resonator cascade on the FFT bins; F = array of 5 formants."""
    f = ear.FREQ[None, :]
    Fk, Bk = F[:, None], BW[:, None] / 2
    h = (Fk ** 2 + Bk ** 2) ** 2 / (((f - Fk) ** 2 + Bk ** 2) * ((f + Fk) ** 2 + Bk ** 2))
    return np.log(h).sum(0)


def score_frames(e):
    """Frames x 7 log-scores by analysis by synthesis; picks the tract length (sets e.Lw)."""
    v = e.voiced & np.isfinite(e.f0)
    sc = np.full((e.T, len(VOWELS)), -np.inf)
    if v.sum() == 0:
        e.Lw = ear.L_REF
        return sc
    # lifter just below the voice's period: keeps the envelope, drops the harmonics
    period = ear.SR / float(np.nanmedian(e.f0[v]))
    cut = max(8, int(0.6 * period))
    obs = _smooth_log(np.log(e.P[v] + 1e-10), cut)
    bins = _bins_of(FGRID)
    O = _flat(10 / np.log(10) * obs[:, bins])                     # dB
    best_tot, best = -np.inf, None
    for L in L_GRID:
        k = ear.L_REF / L
        T = np.array([tube_logspec(np.array([REF[w][0], REF[w][1], F3REF[w], 3500.0, 4500.0]) * k) for w in VOWELS])
        Ts = _flat(10 / np.log(10) * _smooth_log(T, cut)[:, bins])  # 7 x grid
        d = ((O[:, None, :] - Ts[None, :, :]) ** 2).mean(2)          # frames x 7
        ll = -0.5 * K_EFF * d / SIGMA_DB ** 2
        m = ll.max(1, keepdims=True)
        fr = m[:, 0] + np.log(np.exp(ll - m).sum(1))
        tot = 8.0 * fr.mean() - 0.5 * ((L - L_PRIOR) / L_PRIOR_SD) ** 2
        if tot > best_tot:
            best_tot, best = tot, (L, ll)
    e.Lw = float(best[0])
    sc[v] = best[1]
    return sc


METHOD = os.environ.get('WSR_VOWELS', 'formant')   # 'formant' (v1, default) or 'abs' (analysis by synthesis, in progress)


def frame_scores(e):
    if METHOD == 'formant':
        return scores(normalised(e))
    return score_frames(e)


# ------------------------------------------------------------------------------------------------- test set
# espeak-ng with MBROLA voices. The phonemes and durations always come from espeak's it3 transcription (--pho),
# so every voice says exactly the same thing with the same labels; the .pho goes straight to mbrola.
# it3/it4 use espeak's own names (e1 E1 o1 O1 = é è ó ò). it1 has a different inventory with gaps (probed
# 2026-11-03: no A1 I1 U1, no Z, E1 missing after S D, O2 after P T; a missing diphone aborts the utterance, which
# is then skipped). Its names were checked by measuring each one after P T M: E 470/1915 < E1 530/1750 Hz and
# O 490/900 < O1 590/940, so E O are closed and E1 O1 open, as in espeak's convention.
# it2 is left out: its U measures F1 420, F2 2410 Hz, not an /u/ (a broken or mislabelled diphone set).
PHO_V = {'a1': 'a', 'i1': 'i', 'u1': 'u', 'e1': 'e', 'E1': 'ɛ', 'o1': 'o', 'O1': 'ɔ'}
UPPER_V = {'a1': 'A', 'a': 'A2', 'e1': 'E', 'E1': 'E1', 'e': 'E2', 'i1': 'I', 'i': 'I2', 'o1': 'O', 'O1': 'O1',
           'o': 'O1', 'u1': 'U', 'u': 'U2'}
UPPER_C = {'dz': 'dz', 'tS': 'tS', 'dZ': 'dZ', 'ts': 'ts', 'j': 'j', 'J': 'J', 'L': 'L', 'S': 'S', 'r': 'R',
           'z': 'S', 'w': 'w'}
FAMILY = {'mb-it1': 'upper', 'mb-it3': 'lower', 'mb-it4': 'lower'}
ACC = ['à', 'è', 'é', 'ì', 'ò', 'ó', 'ù']               # è ò open, é ó closed (Italian orthography)
SETS = {
    # voice, speed (wpm), time-scale (1.0 = as made; 1.15 = audio played 15% faster: a ~15% shorter tract,
    # the physical effect of a smaller speaker, with higher pitch and formants together)
    'sillabe1': [(v, sp, sc) for v in ('mb-it1', 'mb-it3', 'mb-it4') for sp in (120, 175)
                 for sc in (1.0, 1.15)],
    # fresh set: other consonants, other words, other speeds and sizes (0.95 = a slightly longer tract,
    # 1.25 = a child-sized one)
    'sillabe2': [(v, sp, sc) for v in ('mb-it1', 'mb-it3', 'mb-it4') for sp in (140, 200)
                 for sc in (0.95, 1.08, 1.25)],
}
UTT = {
    'sillabe1': (['', 'p', 't', 'k', 'f', 's', 'm', 'l'],
                 'vita fino pila sera vero mela bene festa sette casa mano pala cosa porta notte sole voce nome '
                 'luna tutto fumo'),
    'sillabe2': (['b', 'd', 'g', 'n', 'v', 'r', 'ci', 'gi'],
                 'riso libro dito pesce neve verde ferro pietra tempo pane fame carta moda rosso forte ora '
                 'monte pollo muro uva sugo'),
}


def utterances(name):
    """Each stressed syllable alone, then the words in short lists (coarticulated, shorter vowels)."""
    cons, words = UTT[name]
    words = words.split()
    for c in cons:
        for v in ACC:
            yield c + v
    for i in range(0, len(words), 7):
        yield ', '.join(words[i:i + 7])


def synth(voice, text, speed, scale, d):
    """(samples, segments) or None if mbrola refused a diphone."""
    pho = subprocess.run(['espeak-ng', '-v', 'mb-it3', '-s', str(speed), '-q', '--pho', text],
                         capture_output=True, text=True).stdout
    lines = [ln.split('\t') for ln in pho.splitlines() if ln.strip()]
    send = []
    for p in lines:
        q = list(p)
        if q[0] != '_':
            # espeak's names that the it diphone sets lack: 'i/' (a non-syllabic i) and doubled consonants 'ss'
            # (the length is already in the duration)
            q[0] = q[0].rstrip('/')
            if len(q[0]) == 2 and q[0][0] == q[0][1] and q[0][0] not in 'aeiouAEIOU':
                q[0] = q[0][0]
        if FAMILY[voice] == 'upper' and q[0] != '_':
            q[0] = UPPER_V.get(q[0]) or UPPER_C.get(q[0]) or q[0].upper()
        send.append('\t'.join(q))
    fp = os.path.join(d, 'x.pho')
    with open(fp, 'w') as f:
        f.write('\n'.join(send) + '\n')
    wav = os.path.join(d, 'x.wav')
    db = f'/usr/share/mbrola/{voice[3:]}/{voice[3:]}'
    r = subprocess.run(['mbrola', db, fp, wav], capture_output=True, text=True)
    if r.returncode != 0 or 'Warning' in r.stderr or 'Fatal' in r.stderr:
        return None
    if scale != 1.0:
        out = os.path.join(d, 'y.wav')
        # the samples read as if recorded at a higher rate, then resampled back to 16 kHz
        subprocess.run(['ffmpeg', '-v', 'quiet', '-y', '-i', wav, '-af',
                        f'asetrate={int(16000 * scale)},aresample={ear.SR}', '-ac', '1', out], check=True)
        wav = out
    segs, t = [], 0.0
    for p in lines:
        if len(p) < 2:
            continue
        dur = float(p[1]) / scale
        segs.append((p[0], t, t + dur))
        t += dur
    return ear.load(wav), segs


def labels(segs, T, family):
    """Per frame: vowel label if the frame centre is in the middle 50% of a stressed vowel segment, else None."""
    lab = [None] * T
    for ph, a, b in segs:
        v = PHO_V.get(ph)
        if v is None:
            continue
        lo, hi = a + 0.25 * (b - a), b - 0.25 * (b - a)
        for t in range(T):
            c = (t * ear.HOP + ear.WIN / 2) / ear.SR * 1000
            if lo <= c <= hi:
                lab[t] = v
    return lab


def run_config(cfg):
    name, voice, speed, scale = cfg
    rows, Ls = [], []
    tag = f'{voice} s{speed} x{scale}'
    with tempfile.TemporaryDirectory() as d:
        if True:
            for text in utterances(name):
                    got = synth(voice, text, speed, scale, d)
                    if got is None:
                        rows.append((tag, 'skip', text))
                        continue
                    x, segs = got
                    e = ear.Ear(x)
                    sc = frame_scores(e)
                    Ls.append(e.Lw)
                    lab = labels(segs, e.T, FAMILY[voice])
                    for t, l in enumerate(lab):
                        if l is None:
                            continue
                        ok = np.isfinite(sc[t]).any()
                        g = VOWELS[int(sc[t].argmax())] if ok else None
                        rows.append((tag, l, g))
    return tag, rows, Ls


def test(name):
    import multiprocessing
    rows, Ls = [], {}
    with multiprocessing.Pool() as pool:
        for tag, r, l in pool.map(run_config, [(name,) + c for c in SETS[name]]):
            rows += r
            Ls[tag] = l
    return rows, Ls


def report(rows, Ls):
    skipped = [r for r in rows if r[1] == 'skip']
    rows = [r for r in rows if r[1] != 'skip']
    def acc(sel, five=False):
        n = len(sel)
        if not n:
            return 'n/a'
        if five:
            k = sum(1 for _, l, g in sel if g is not None and FIVE[g] == FIVE.get(l, l.strip('()')))
        else:
            sel = [r for r in sel if not r[1].startswith('(')]
            n = len(sel)
            if not n:
                return '  n/a'
            k = sum(1 for _, l, g in sel if g == l)
        return f'{100 * k / n:5.1f}%'
    out = []
    out.append(f'skipped utterances (mbrola refused): {len(skipped)} ' + ' '.join(sorted(set(r[2] for r in skipped)))[:200])
    out.append(f'frames {len(rows)}; no formants {sum(1 for r in rows if r[2] is None)}')
    out.append(f'all: 7-way {acc(rows)}  5-way {acc(rows, True)}')
    for tag in dict.fromkeys(r[0] for r in rows):
        sel = [r for r in rows if r[0] == tag]
        out.append(f'  {tag:22s} L {np.median(Ls[tag]):4.1f} ({np.percentile(Ls[tag], 10):4.1f}-{np.percentile(Ls[tag], 90):4.1f}) cm  7-way {acc(sel)}  5-way {acc(sel, True)}')
    out.append('per vowel (7-way / 5-way):')
    for v in VOWELS + ['(e)', '(o)']:
        sel = [r for r in rows if r[1] == v]
        out.append(f'  {v:3s}  {acc(sel)}  {acc(sel, True)}  ({len(sel)} frames)')
    out.append('confusion (rows true, cols guess):')
    out.append('     ' + ' '.join(f'{v:>5s}' for v in VOWELS + ['-']))
    for v in VOWELS + ['(e)', '(o)']:
        sel = [r for r in rows if r[1] == v]
        cnt = [sum(1 for r in sel if r[2] == g) for g in VOWELS + [None]]
        out.append(f'  {v:3s}' + ' '.join(f'{c:5d}' for c in cnt))
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--test', action='store_true')
    ap.add_argument('--set', default='sillabe1')
    a = ap.parse_args()
    if a.test:
        rows, Ls = test(a.set)
        r = report(rows, Ls)
        print(r)
        print(f'\n(set {a.set}, {datetime.date.today()})')


if __name__ == '__main__':
    main()
