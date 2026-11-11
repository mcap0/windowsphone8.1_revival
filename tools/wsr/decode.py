"""wsr decode (phase 4, first version): choose among grammar sentences by analysis by synthesis.

Every candidate sentence is said by MBROLA it3 (a male voice; the Lumia reference speaker is male) and turned into
utterance-normalised MFCC (c1-c12, mean and spread over its speech frames removed: microphone, room and much of
the speaker cancel). A recording's cost against a candidate is the DTW distance (C, tools/wsr/dtw.so) per path step;
the best candidate wins, and the gap to the second is the confidence. No training: the templates are synthesized
from the text.

Candidates: tools/wsp/golden.tsv, 1000 grammar samples (tools/wsp/grammar.py) and the commands of the
recording prompts. Cache: share/wsr-cache/<hash>.npy.

    python3 tools/wsr/decode.py <session> [first last]     e.g. 20261103-151634-speaker 23 50
"""
import ctypes
import hashlib
import multiprocessing
import os
import re
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'wsp'))
import dtwalign  # noqa: E402
import ear  # noqa: E402
import vowels as V  # noqa: E402

ROOT = os.path.join(HERE, '..', '..', 'share')
CACHE = os.path.join(ROOT, 'wsr-cache')
_lib = ctypes.CDLL(os.path.join(HERE, 'dtw.so'))
_lib.dtw_cost.restype = ctypes.c_double
_lib.dtw_cost.argtypes = [ctypes.POINTER(ctypes.c_double), ctypes.c_int, ctypes.POINTER(ctypes.c_double),
                          ctypes.c_int, ctypes.c_int, ctypes.c_double]


MAP = os.environ.get('WSR_MAP', '0') == '1'   # apply the MBROLA -> speaker map (speakermap.py) to the templates
DELTA_W = float(os.environ.get('WSR_DELTA', '0'))   # weight of the deltas in the DTW distance (0 = none)


def dfeats(x):
    """Decoder features: dtwalign's normalised c1-c12, plus their deltas (normalised the same way) x DELTA_W:
    consonants are in the changes, not in the steady parts."""
    e, c = dtwalign.feats(x)
    if DELTA_W <= 0:
        return e, c
    d = ear.deltas(c)
    sp = e.energy > e.floor + 15
    d /= d[sp].std(0) + 1e-6 if sp.any() else 1.0
    return e, np.hstack([c, DELTA_W * d])


def norm(text):
    return re.sub(r'[^a-zàèéìòù0-9 ]', ' ', text.lower()).split()


def candidates():
    import grammar
    out = []
    for line in open(os.path.join(HERE, '..', 'wsp', 'golden.tsv'), encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        out.append(line.split('\t')[0])
    out += [t for t, _ in grammar.samples(1000, seed=3)]
    sess = os.path.join(ROOT, 'voce', '20261103-151634-speaker', 'elenco.tsv')
    if os.path.exists(sess):
        for line in open(sess, encoding='utf-8'):
            p = line.split('\t')
            if len(p) > 1 and int(p[0][:3]) >= 23:
                out.append(p[1])
    cmd = os.path.join(HERE, '..', '..', 'apps', 'Assistente', 'Assistente', 'Assets', 'comandi.txt')
    for line in open(cmd, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) > 1:
            out.append(p[1])
    cmd = os.path.join(HERE, '..', '..', 'apps', 'Assistente', 'Assistente', 'Assets', 'comandi.txt')
    for line in open(cmd, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) > 1:
            out.append(p[1])
    seen, uniq = set(), []
    for t in out:
        k = ' '.join(norm(t))
        if k and k not in seen:
            seen.add(k)
            uniq.append(t)
    return uniq


def template(text):
    key = hashlib.sha1((' '.join(norm(text)) + f'|d{DELTA_W}').encode()).hexdigest()[:16]
    if MAP:
        F = template_raw(text, key)
        if F is None:
            return None
        import speakermap
        return np.ascontiguousarray(speakermap.apply(F, speakermap.load()))
    return template_raw(text, key)


def template_raw(text, key):
    path = os.path.join(CACHE, key + '.npy')
    if os.path.exists(path):
        return np.load(path)
    with tempfile.TemporaryDirectory() as d:
        got = V.synth('mb-it3', ' '.join(norm(text)), 150, 1.0, d)
    if got is None:
        return None
    e, F = dfeats(got[0])
    a, b = dtwalign.speech_span(e)
    F = np.ascontiguousarray(F[a:b])
    np.save(path, F)
    return F


def _tpl(text):
    return text, template(text)


def cost(A, B, band=0.4):
    return _lib.dtw_cost(A.ctypes.data_as(ctypes.POINTER(ctypes.c_double)), len(A),
                         B.ctypes.data_as(ctypes.POINTER(ctypes.c_double)), len(B), A.shape[1], band)


def load_templates():
    os.makedirs(CACHE, exist_ok=True)
    with multiprocessing.Pool() as pool:
        got = pool.map(_tpl, candidates())
    return [(t, F) for t, F in got if F is not None and len(F) > 5]


def decode(x, tpls):
    e, F = dfeats(x)
    a, b = dtwalign.speech_span(e)
    A = np.ascontiguousarray(F[a:b])
    scored = sorted((cost(A, B), t) for t, B in tpls)
    return scored


BETA = float(os.environ.get('WSR_BETA', '0.15'))
TOPK = 20


def vowel_evidence(x, text, model):
    """Mean log posterior of the expected vowel over the vowel segments, after aligning x to the candidate."""
    import fit
    import mfccmodel as M
    r = dtwalign.align(x, ' '.join(norm(text)))
    if r is None:
        return -5.0
    segs, e = r
    L = model.ll(M.features(e))
    tot, n = 0.0, 0
    for ph, a, b in segs:
        v = fit.LAB.get(ph)
        if v is None or b - a < 3 or v not in model.vowels:
            continue
        fr = list(range(a + (b - a) // 4, max(b - (b - a) // 4, a + (b - a) // 4 + 1)))
        S = L[fr].sum(0) + model.logprior
        post = S - np.logaddexp.reduce(S)
        # 5-way: the expected vowel's group (e+ɛ, o+ɔ)
        grp = [j for j, w in enumerate(model.vowels) if V.FIVE[w] == V.FIVE[v]]
        tot += float(np.logaddexp.reduce(post[grp]))
        n += 1
    return tot / max(n, 1)


def rescore(x, scored, model):
    """Top TOPK by DTW, re-ranked by DTW cost - BETA * vowel evidence."""
    out = []
    for c, t in scored[:TOPK]:
        out.append((c - BETA * vowel_evidence(x, t, model), t))
    return sorted(out) + scored[TOPK:]


def main():
    session = sys.argv[1]
    lo, hi = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (1, 999)
    if session in ('--test', '--dev'):
        # --test: the 50 even command takes (fit.dataset('test')); --dev: the odd ones (tuning)
        import fit
        if session == '--test':
            items = fit.dataset('test')
        else:
            items = [t for t in fit.dataset('fit') if '-comandimatte' in t[0]]
        return run(items)
    d = os.path.join(ROOT, 'voce', session)
    items = []
    for line in open(os.path.join(d, 'elenco.tsv'), encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 2 and lo <= int(p[0][:3]) <= hi:
            items.append((os.path.join(d, p[0]), p[1], int(p[0][:3])))
    run(items)


def run(items):
    tpls = load_templates()
    print(len(tpls), 'candidate sentences')
    import mfccmodel as M
    model = M.Model()
    ok = n = 0
    ranks = []
    for path, said, k in items:
        p = (os.path.basename(path), said)
        want = ' '.join(norm(p[1]))
        x = ear.load(path)
        sc = decode(x, tpls)
        if BETA > 0:
            sc = rescore(x, sc, model)
        names = [' '.join(norm(t)) for _, t in sc]
        r = names.index(want) + 1 if want in names else -1
        ranks.append(r)
        n += 1
        ok += r == 1
        gap = sc[1][0] - sc[0][0]
        print(f'{p[0]} {"OK " if r == 1 else "   "} rank {r:4d}  gap {gap:.3f}  "{p[1]}" -> "{sc[0][1]}"')
    print(f'right sentence first: {ok}/{n}; in the top 5: {sum(1 for r in ranks if 0 < r <= 5)}/{n}')


if __name__ == '__main__':
    main()
