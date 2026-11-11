"""wsr vowel model on MFCC, fitted on the Lumia (user's permission, 2026-11-03), with per-utterance VTLN.

Model: one diagonal Gaussian per vowel on c1-c12 + deltas, cepstral mean removed over the utterance's speech
frames (that removes the microphone and the room). Fitted on the odd takes of a reference speaker's reading
(speaker); the variances are floored at 0.5x their mean over vowels (hand choice: few frames per vowel otherwise
make some vowels too narrow).

Other speakers (the hand-written part): vocal tract length normalisation on the filterbank. The mel triangles are
laid on f * alpha instead of f (alpha < 1 for shorter tracts: women, children), for alpha in 0.78..1.12; per
utterance the alpha whose features fit the reference vowels best (log-sum-exp over vowels, voiced frames) wins,
with a written prior N(1, 0.08) on alpha (adult tracts 14.5-17.5 cm around a 16-17 cm reference).

    python3 tools/wsr/mfccmodel.py fit speaker                  -> tools/wsr/vocali-mfcc.npz
    python3 tools/wsr/mfccmodel.py test speaker|mamma [--novtln]  even takes for the fit speaker, all for others
    python3 tools/wsr/mfccmodel.py synth [--novtln]            MBROLA it1 / it3 / it4 reading the same text
"""
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear  # noqa: E402
import fit  # noqa: E402
import vowels as V  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(HERE, 'vocali-mfcc.npz')
ALPHAS = np.round(np.arange(0.78, 1.121, 0.02), 3)
ALPHA_SD = 0.08
SMOOTH = int(os.environ.get('WSR_SMOOTH', '5'))
FIVE_DECIDE = os.environ.get('WSR_FIVE', '1') == '1'
_MEL = {}


def mel_for(alpha):
    if alpha not in _MEL:
        # triangles placed at f*alpha: a frame of a shorter tract is read as if from the reference speaker
        edges = ear.mel_to_hz(np.linspace(ear.hz_to_mel(0.0), ear.hz_to_mel(ear.SR / 2), ear.NMEL + 2))
        edges = np.minimum(edges / alpha, ear.SR / 2) if alpha < 1 else edges / alpha
        f = ear.FREQ
        M = np.zeros((ear.NMEL, len(f)))
        for i in range(ear.NMEL):
            a, b, c = edges[i], edges[i + 1], edges[i + 2]
            M[i] = np.clip(np.minimum((f - a) / max(b - a, 1e-6), (c - f) / max(c - b, 1e-6)), 0, None)
        _MEL[alpha] = M
    return _MEL[alpha]


def features(e, alpha=1.0):
    c = np.log(e.P @ mel_for(alpha).T + 1e-10) @ ear.DCT.T
    F = np.hstack([c[:, 1:13], ear.deltas(c)[:, 1:13]])
    if SMOOTH > 1:
        # edit B: a vowel's colour changes slowly; the mean over SMOOTH frames (centred) lowers each frame's noise
        k = SMOOTH // 2
        P = np.pad(F, ((k, k), (0, 0)), mode='edge')
        F = sum(P[j:j + len(F)] for j in range(SMOOTH)) / SMOOTH
    sp = e.energy > e.floor + 15
    return F - (F[sp].mean(0) if sp.any() else F.mean(0))


TEXT = os.path.join(HERE, os.environ.get('WSR_VOCALI', 'vocali.txt'))


def load_text(path=TEXT):
    """The hand-editable model: 'centre v c1..c12', 'width w1..w12', 'prior v=x ...'."""
    vow, mu, width, prior = [], [], None, {}
    for line in open(path, encoding='utf-8'):
        p = line.split('#')[0].split()
        if not p:
            continue
        if p[0] == 'centre':
            vow.append(p[1])
            mu.append([float(x) for x in p[2:14]])
        elif p[0] == 'width':
            width = [float(x) for x in p[1:13]]
        elif p[0] == 'prior':
            prior = {k: float(x) for k, x in (q.split('=') for q in p[1:])}
    mu = np.array(mu)
    sd = np.tile(np.array(width), (len(vow), 1))
    # the feature vector also holds 12 deltas: zero mean, infinite width (no information)
    mu = np.hstack([mu, np.zeros_like(mu)])
    sd = np.hstack([sd, np.full_like(sd, 1e6)])
    return vow, mu, sd, np.array([prior.get(v, 0.0) for v in vow])


class Model:
    def __init__(self, path=MODEL):
        if os.path.exists(TEXT) and os.environ.get('WSR_MODEL', 'text') == 'text':
            self.vowels, self.mu, self.sd, self.logprior = load_text()
            return
        z = np.load(path, allow_pickle=True)
        self.vowels = list(z['vowels'])
        self.mu, self.sd = z['mu'], z['sd']
        self.logprior = z['logprior'] if 'logprior' in z else np.zeros(len(self.vowels))

    def ll(self, F):
        return (-0.5 * (((F[:, None, :] - self.mu[None]) / self.sd[None]) ** 2).sum(2)
                - np.log(self.sd).sum(1)[None])

    def warp(self, e):
        """alpha for this utterance (voiced frames), by likelihood + prior."""
        v = e.voiced
        if v.sum() < 5:
            return 1.0
        best, ba = -np.inf, 1.0
        for a in ALPHAS:
            L = self.ll(features(e, a)[v])
            m = L.max(1, keepdims=True)
            fr = m[:, 0] + np.log(np.exp(L - m).sum(1))
            # frame scores averaged, x 8 effective looks (as for the formant warp), plus the prior
            tot = 8.0 * fr.mean() / 24.0 - 0.5 * ((a - 1.0) / ALPHA_SD) ** 2
            if tot > best:
                best, ba = tot, a
        return float(ba)

    def classify(self, e, vtln=True):
        a = self.warp(e) if vtln else 1.0
        L = self.ll(features(e, a))
        return np.array(self.vowels)[L.argmax(1)], a


def do_fit(spk):
    X, y = [], []
    tk = fit.dataset('fit') if spk == 'all' else [t for t in fit.takes(spk) if t[2] % 2 == 1]
    for path, text, k in tk:
        vs, e = fit.labelled(path, text)
        if e is None:
            continue
        F = features(e)
        for v, fr in vs:
            X += [F[t] for t in fr]
            y += [v] * len(fr)
    X, y = np.array(X), np.array(y)
    vs = [v for v in V.VOWELS if (y == v).sum() >= 10]
    mu = np.array([X[y == v].mean(0) for v in vs])
    sd = np.array([X[y == v].std(0) for v in vs])
    sd = np.maximum(sd, 0.5 * sd.mean(0, keepdims=True))
    np.savez(MODEL, vowels=np.array(vs), mu=mu, sd=sd)
    print('fit:', {v: int((y == v).sum()) for v in vs}, '->', MODEL)


def score(rows):
    n = len(rows)
    a7 = sum(g == l for l, g in rows) / n * 100
    a5 = sum(V.FIVE[g] == V.FIVE[l] for l, g in rows) / n * 100
    per = {}
    for l, g in rows:
        per.setdefault(V.FIVE[l], []).append(V.FIVE[g] == V.FIVE[l])
    return f'{n} frames, 5-way {a5:.1f}%, 7-way {a7:.1f}%  (' + ' '.join(
        f'{v} {100 * np.mean(per[v]):.0f}' for v in 'ieaou' if v in per) + ')'


def do_test(spk, vtln, show=False):
    m = Model()
    rows, alphas, segrows, bad = [], [], [], []
    tk = (fit.dataset('test') if spk == 'all' else
          [t for t in fit.dataset('fit') if '-comandimatte' in t[0]] if spk == 'dev' else fit.takes(spk))
    for path, text, k in tk:
        if spk == 'speaker' and k % 2 == 1:  # the fit speaker: only the even (held-out) takes
            continue
        vs, e = fit.labelled(path, text)
        if e is None:
            continue
        g, a = m.classify(e, vtln)
        alphas.append(a)
        rows += [(v, g[t]) for v, fr in vs for t in fr]
        L = m.ll(features(e, a))
        for v, fr in vs:
            # segment decision: the vowel with the highest summed log-likelihood over the segment's frames
            S = L[fr].sum(0) + m.logprior
            if FIVE_DECIDE:
                # edit D: decide among the 5 written vowels, e+ɛ and o+ɔ summed (log-sum-exp)
                groups = {}
                for j, w in enumerate(m.vowels):
                    groups.setdefault(V.FIVE[w], []).append(S[j])
                best5 = max(groups, key=lambda g: np.logaddexp.reduce(groups[g]))
                gs = max((w for w in m.vowels if V.FIVE[w] == best5), key=lambda w: S[m.vowels.index(w)])
            else:
                gs = m.vowels[int(S.argmax())]
            segrows.append((v, gs))
            if V.FIVE[gs] != V.FIVE[v]:
                bad.append((k, v, gs, fr[0] * 10, text))
    print(f'{spk} frames:   {score(rows)}  alpha median {np.median(alphas):.2f}')
    print(f'{spk} segments: {score(segrows)}')
    if show:
        for b in bad[:40]:
            print(f'   take {b[0]:3d} at {b[3]:5d} ms: {b[1]} -> {b[2]}   "{b[4]}"')


def do_synth(vtln):
    m = Model()
    lines = [l.split('\t')[1].strip() for l in open(os.path.join(HERE, '..', '..', 'apps', 'Assistente',
                                                               'Assistente', 'Assets', 'testo.txt'), encoding='utf-8')]
    lab = dict(fit.LAB)
    with tempfile.TemporaryDirectory() as d:
        for voice, scale in (('mb-it3', 1.0), ('mb-it1', 1.0), ('mb-it4', 1.0), ('mb-it3', 1.15), ('mb-it4', 1.15)):
            rows, alphas = [], []
            for text in lines[::3]:
                got = V.synth(voice, text, 150, scale, d)
                if got is None:
                    continue
                x, segs = got
                e = ear.Ear(x)
                g, a = m.classify(e, vtln)
                alphas.append(a)
                for ph, s, t in segs:
                    v = lab.get(ph if voice != 'mb-it1' else ph)
                    if v is None or t - s < 50:
                        continue
                    f0, f1 = int((s + (t - s) / 4) / 10), int((t - (t - s) / 4) / 10)
                    rows += [(v, g[k]) for k in range(f0, min(f1, e.T))]
            print(f'{voice} x{scale}: {score(rows)}  alpha median {np.median(alphas):.2f}')


if __name__ == '__main__':
    cmd = sys.argv[1]
    novtln = '--novtln' in sys.argv
    if cmd == 'fit':
        do_fit(sys.argv[2])
    elif cmd == 'test':
        do_test(sys.argv[2], not novtln, '--show' in sys.argv)
    elif cmd == 'synth':
        do_synth(not novtln)
