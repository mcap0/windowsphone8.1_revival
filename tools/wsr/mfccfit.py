"""Quick test: one diagonal Gaussian per vowel on utterance-normalised MFCC (c1-c12 + deltas), fitted on the odd
takes of a speaker's reading, scored on the even takes (and on other speakers with --other). Labels: dtwalign.

    python3 tools/wsr/mfccfit.py speaker [--other mamma]
"""
import sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import fit  # noqa: E402
import vowels as V  # noqa: E402

FIVE = V.FIVE


def feats(e):
    c = np.hstack([e.mfcc[:, 1:13], e.mfcc[:, 14:26]])
    sp = e.energy > e.floor + 15
    m = c[sp].mean(0) if sp.any() else c.mean(0)
    return c - m


def gather(tk):
    X, y = [], []
    for path, text, k in tk:
        vs, e = fit.labelled(path, text)
        if e is None:
            continue
        F = feats(e)
        for v, fr in vs:
            for t in fr:
                X.append(F[t]); y.append(v)
    return np.array(X), np.array(y)


def main():
    spk = sys.argv[1]
    tk = fit.takes(spk)
    Xa, ya = gather([t for t in tk if t[2] % 2 == 1])
    Xb, yb = gather([t for t in tk if t[2] % 2 == 0])
    vs = [v for v in V.VOWELS if (ya == v).sum() >= 10]
    mu = np.array([Xa[ya == v].mean(0) for v in vs])
    sd = np.array([Xa[ya == v].std(0) + 1e-3 for v in vs])
    def classify(X):
        ll = -0.5 * (((X[:, None, :] - mu[None]) / sd[None]) ** 2).sum(2) - np.log(sd).sum(1)[None]
        return np.array(vs)[ll.argmax(1)]
    for name, X, y in [('even takes ' + spk, Xb, yb)] + [('other ' + o, *gather(fit.takes(o))) for o in sys.argv[3:]]:
        g = classify(X)
        a7 = (g == y).mean() * 100
        a5 = np.mean([FIVE[a] == FIVE[b] for a, b in zip(g, y)]) * 100
        print(f'{name}: {len(y)} frames, 7-way {a7:.1f}%, 5-way {a5:.1f}%')
        for v in vs:
            m = y == v
            if m.any():
                print(f'   {v}: {np.mean([FIVE[a] == FIVE[v] for a in g[m]]) * 100:5.1f}% ({m.sum()})')


if __name__ == '__main__':
    main()
