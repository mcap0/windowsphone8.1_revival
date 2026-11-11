"""Hand-read vowel spectra (spettri-speaker.txt) -> model centres, with the ear's fixed formulas only:
power on the FFT bins from the straight-line dB curve, mel triangles, log, DCT -> c1..c12. The utterance mean is
not known for a single vowel: the centres keep the mean over vowels of the current model (vocali.txt), so only
the differences between vowels come from the hand-read curves. Widths and priors are kept.

    python3 tools/wsr/spettri.py > tools/wsr/vocali-spettro.txt
"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ear, mfccmodel as M  # noqa: E401,E402

cur_v, cur_mu, cur_sd, cur_lp = M.load_text()
curves = {}
for line in open(os.path.join(HERE, 'spettri-speaker.txt'), encoding='utf-8'):
    p = line.split('#')[0].split()
    if p:
        pts = [tuple(float(x) for x in q.split(':')) for q in p[1:]]
        curves[p[0]] = np.interp(ear.FREQ, [a for a, _ in pts], [b for _, b in pts])
C = {v: (np.log(10 ** (curves[v] / 10) @ ear.MEL.T + 1e-10) @ ear.DCT.T)[1:13] for v in cur_v}
mean_hand = np.mean([C[v] for v in cur_v], 0)
mean_cur = cur_mu[:, :12].mean(0)
print('# centres from the hand-read spectra (spettri-speaker.txt via spettri.py); widths and priors from vocali.txt')
for v in cur_v:
    print('centre ' + v + ' ' + ' '.join(f'{x:6.1f}' for x in C[v] - mean_hand + mean_cur))
print('width ' + ' '.join(f'{x:6.1f}' for x in cur_sd[0, :12]))
print('prior ' + ' '.join(f'{v}={x:.2f}' for v, x in zip(cur_v, cur_lp)))
