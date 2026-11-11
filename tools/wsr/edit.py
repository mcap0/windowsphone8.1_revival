"""Hand edits of the fitted vowel model (vocali-mfcc.fit.npz -> vocali-mfcc.npz), each with its reason.
Measured with  python3 tools/wsr/mfccmodel.py test speaker  (even takes, never fitted). Log in MISURE.md.

    python3 tools/wsr/edit.py A B ...     apply the edits in order
"""
import sys
import numpy as np
import os

HERE = os.path.dirname(os.path.abspath(__file__))
z = dict(np.load(os.path.join(HERE, 'vocali-mfcc.fit.npz'), allow_pickle=True))
vow = list(z['vowels'])
mu, sd = z['mu'].copy(), z['sd'].copy()
logprior = np.zeros(len(vow))
N = {'i': 168, 'e': 155, 'ɛ': 65, 'a': 251, 'ɔ': 54, 'o': 237, 'u': 103}   # fit frames per vowel


def pooled():
    w = np.array([N[v] for v in vow], float)[:, None]
    return np.sqrt((w * sd ** 2).sum(0) / w.sum())


for ed in sys.argv[1:]:
    if ed == 'A':
        # deltas: mean ~0 for every vowel (they measure change, not quality) -> no information, only the -log sd
        # bias; one width for all vowels (tied): few frames for ɔ ɛ made them narrow and they won too often
        mu[:, 12:] = 0.0
        sd[:] = pooled()[None, :]
        sd[:, 12:] = 1e6
    if ed == 'C':
        # vowel priors from Italian letter frequencies (a 11.7, e 11.8, i 11.3, o 9.8, u 3.0 %): u is rare; closed
        # e o are ~4x the open ones (every unstressed one is closed)
        P = {'i': 11.3, 'e': 11.8 * 0.8, 'ɛ': 11.8 * 0.2, 'a': 11.7, 'ɔ': 9.8 * 0.2, 'o': 9.8 * 0.8, 'u': 3.0}
        logprior[:] = [np.log(P[v] / sum(P.values())) for v in vow]
    if ed == 'E':
        # high cepstra (fine spectral detail: harmonics, noise, the speaker) pushed most of the remaining errors
        # (a<->o by c8 c11, e->a by c9 c10, e->u by c8) and had the narrowest widths, so the most weight
        sd[:, 8:12] *= 1.5
        sd[:, 7] *= 1.3
    if ed == 'G':
        # c2, c3: the two dims that separate the vowels best (between/within 1.09, 0.98): more weight
        sd[:, 1:3] *= 0.85
np.savez(os.path.join(HERE, 'vocali-mfcc.npz'), vowels=np.array(vow), mu=mu, sd=sd, logprior=logprior)
print('edits', ' '.join(sys.argv[1:]) or '(none)')
