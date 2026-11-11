"""Average smoothed spectrum of each of speaker's stressed vowels (reading takes, DTW labels), one panel per vowel,
for reading the formants by eye. python3 tools/wsr/vowelplot.py out.png"""
import sys, os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear, fit, dtwalign  # noqa: E402

STRESSED = {'a1': 'a', 'i1': 'i', 'u1': 'u', 'e1': 'e', 'E1': 'ɛ', 'o1': 'o', 'O1': 'ɔ'}
spec = {v: [] for v in STRESSED.values()}
for path, text, k in fit.dataset('fit'):
    if '-testospeaker' not in path:
        continue
    r = dtwalign.align(ear.load(path), text)
    if r is None:
        continue
    segs, e = r
    logP = np.log(e.P + 1e-10)
    # cepstral smoothing below the pitch period (~120 Hz voice): keep quefrency < 5 ms
    c = np.fft.irfft(logP, ear.NFFT); c[:, 80:ear.NFFT - 79] = 0
    S = 10 / np.log(10) * np.fft.rfft(c, ear.NFFT).real[:, :ear.NFFT // 2 + 1]
    for ph, a, b in segs:
        v = STRESSED.get(ph)
        if v and b - a >= 6:
            fr = range(a + (b - a) // 4, b - (b - a) // 4)
            spec[v] += [S[t] - S[t].max() for t in fr if e.voiced[t]]
f = ear.FREQ
fig, axes = plt.subplots(7, 1, figsize=(10, 16), sharex=True)
for ax, v in zip(axes, ['i', 'e', 'ɛ', 'a', 'ɔ', 'o', 'u']):
    X = np.array(spec[v])
    if len(X):
        m = X.mean(0)
        ax.plot(f, m, 'k')
        ax.fill_between(f, m - X.std(0), m + X.std(0), color='0.85')
    ax.set_ylabel(f'/{v}/  n={len(X)}', rotation=0, labelpad=40, fontsize=13)
    ax.set_xticks(np.arange(0, 5001, 250)); ax.grid(True, which='both', alpha=0.5)
    ax.set_xlim(0, 5000); ax.set_ylim(-45, 2)
axes[-1].set_xlabel('Hz')
fig.suptitle('speaker, Lumia, stressed vowels: mean smoothed spectrum (dB, peak = 0), band = 1 sd')
fig.tight_layout(); fig.savefig(sys.argv[1], dpi=70)
print('ok')
