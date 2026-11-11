"""wsr ear: fixed-formula front end (VOCE-PIANO.md 4.1).

Every weight here is written by definition (cosines of the DFT, mel triangles, DCT, Hamming window, the
pre-emphasis pair) or comes from acoustic phonetics (tube resonances for the vocal-tract length). Nothing is
fitted to recordings. The only per-utterance quantities (noise floor, speaker tract length) are computed at run
time from the utterance itself, like a normalisation layer, never from other data.

    python3 tools/wsr/ear.py file.wav       prints a frame table (energy, voicing, F0, F1-F3, L)
"""
import math
import os
import subprocess
import sys
import wave

import numpy as np

SR = 16000
WIN = 400            # 25 ms
HOP = 160            # 10 ms
NFFT = 512
NMEL = 40
NCEP = 13
PRE = 0.97           # pre-emphasis x[n] - 0.97 x[n-1]

# Selective linear prediction (Makhoul 1975): LPC fitted only to the 0..CEIL part of the power spectrum, so a
# 12-pole model holds the 5 resonances below the ceiling plus one pole pair for the glottal and radiation tilt
# (the classic rule: 2 poles per kHz of band, plus 2). Without the spare pair, F1 and F2 of back vowels (/u o ɔ/,
# 300 and 700 Hz) merge into one pole. 5500 Hz is the usual first-pass ceiling (Praat's default for women).
CEIL = 5500.0
LPC_ORDER = 12

# Vocal-tract length. A uniform tube closed at the glottis, open at the lips resonates at (2k-1) c / 4L.
SOUND_CM_S = 35000.0  # speed of sound in warm, moist air
L_REF = 17.5          # adult male reference tract, cm (F1..F4 = 500, 1500, 2500, 3500 Hz)


# ---------------------------------------------------------------------------------------------------- audio
def load(path):
    """16 kHz mono float in [-1, 1]. Other rates go through ffmpeg (a resampler, not a model)."""
    with wave.open(path, 'rb') as w:
        ok = w.getframerate() == SR and w.getnchannels() == 1 and w.getsampwidth() == 2
        if ok:
            return np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.float64) / 32768.0
    raw = subprocess.run(['ffmpeg', '-v', 'quiet', '-i', path, '-ac', '1', '-ar', str(SR), '-f', 's16le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0


def frames(x, win=WIN, hop=HOP):
    """Frame matrix, frame t centred at t*hop + win/2 (zero-padded at the end)."""
    n = max(1, 1 + (len(x) - win + hop - 1) // hop)
    pad = np.zeros((n - 1) * hop + win)
    pad[:len(x)] = x[:len(pad)]
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    return pad[idx]


# ------------------------------------------------------------------------------------------- fixed matrices
def hamming(n):
    return 0.54 - 0.46 * np.cos(2 * np.pi * np.arange(n) / (n - 1))


def dft_matrices(n=NFFT, win=WIN):
    """Real and imaginary DFT rows: W[k, m] = cos(2 pi k m / N), -sin(...). Bins 0..N/2."""
    k = np.arange(n // 2 + 1)[:, None]
    m = np.arange(win)[None, :]
    a = 2 * np.pi * k * m / n
    return np.cos(a), -np.sin(a)


def hz_to_mel(f):
    return 2595.0 * np.log10(1.0 + np.asarray(f) / 700.0)


def mel_to_hz(m):
    return 700.0 * (10.0 ** (np.asarray(m) / 2595.0) - 1.0)


def mel_matrix(nmel=NMEL, n=NFFT, lo=0.0, hi=SR / 2):
    """Triangles equally spaced on the mel scale, peak 1."""
    edges = mel_to_hz(np.linspace(hz_to_mel(lo), hz_to_mel(hi), nmel + 2))
    f = np.arange(n // 2 + 1) * SR / n
    M = np.zeros((nmel, len(f)))
    for i in range(nmel):
        a, b, c = edges[i], edges[i + 1], edges[i + 2]
        M[i] = np.clip(np.minimum((f - a) / (b - a), (c - f) / (c - b)), 0, None)
    return M


def dct_matrix(ncep=NCEP, nmel=NMEL):
    """DCT-II rows: C[q, i] = cos(pi q (i + 0.5) / nmel)."""
    q = np.arange(ncep)[:, None]
    i = np.arange(nmel)[None, :]
    return np.cos(np.pi * q * (i + 0.5) / nmel)


HAM = hamming(WIN)
DRE, DIM = dft_matrices()
MEL = mel_matrix()
DCT = dct_matrix()
FREQ = np.arange(NFFT // 2 + 1) * SR / NFFT


def deltas(c, k=2):
    """Regression slope over +-k frames (the usual delta formula)."""
    pad = np.pad(c, ((k, k), (0, 0)), mode='edge')
    num = sum(j * (pad[k + j:len(c) + k + j] - pad[k - j:len(c) + k - j]) for j in range(1, k + 1))
    return num / (2 * sum(j * j for j in range(1, k + 1)))


# ----------------------------------------------------------------------------------------------- the layers
def spectrum(x):
    """Power spectrum per frame (pre-emphasis, Hamming, DFT as two matrix products)."""
    y = np.append(x[:1], x[1:] - PRE * x[:-1])
    F = frames(y) * HAM
    re, im = F @ DRE.T, F @ DIM.T
    return re * re + im * im


def mfcc(P):
    logmel = np.log(P @ MEL.T + 1e-10)
    c = logmel @ DCT.T
    return np.hstack([c, deltas(c), deltas(deltas(c))]), logmel


def log_energy(x):
    F = frames(x)
    return 10 * np.log10((F * F).mean(1) + 1e-12)


def periodicity(x, fmin=60.0, fmax=450.0):
    """Boersma (1993), the method of Praat: a Hann window of 3 periods of fmin (50 ms), autocorrelation of the
    windowed frame divided by the autocorrelation of the window itself, so a perfectly periodic signal gives 1 at
    its period whatever the lag. Returns (strength 0..1, F0 Hz); frames centred like the 25 ms ones."""
    win = int(3 * SR / fmin)
    win += win % 2
    off = (win - WIN) // 2
    xp = np.pad(x, (off, off + win))
    F = frames(xp, win=win)[:len(frames(x))]
    F = F - F.mean(1, keepdims=True)
    w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(win) / win)
    lo, hi = int(SR / fmax), int(SR / fmin)
    n = 1 << int(np.ceil(np.log2(2 * win)))           # linear, not circular, correlation
    S = np.fft.rfft(F * w, n)
    ra = np.fft.irfft(S * np.conj(S), n)[:, :hi + 2]
    W = np.fft.rfft(w, n)
    rw = np.fft.irfft(W * np.conj(W), n)[:hi + 2]
    rn = ra / (ra[:, :1] + 1e-12) / (rw / rw[0])[None, :]
    seg = rn[:, lo:hi + 1]
    j = seg.argmax(1)
    strength = np.clip(seg[np.arange(len(seg)), j], 0, 1)
    # a maximum on the edge of the lag range is a slope (a low thump or DC step), not a period
    strength = np.where((j == 0) | (j == seg.shape[1] - 1), 0.0, strength)
    jj = np.clip(j + lo, 1, hi)
    r = np.arange(len(rn))
    a, b, c = rn[r, jj - 1], rn[r, jj], rn[r, jj + 1]
    den = a - 2 * b + c
    d = 0.5 * (a - c) / np.where(np.abs(den) < 1e-9, -1e-9, den)
    f0 = SR / (jj + np.clip(d, -1, 1))
    return strength, f0


def lpc_from_spectrum(P, ceil=CEIL, order=LPC_ORDER):
    """Selective LPC: the 0..ceil band is treated as a full band, autocorrelation = inverse DFT of its power,
    then Levinson-Durbin. Returns predictor polys a (frames x order+1) and the band's sample rate (2*ceil)."""
    nb = int(ceil / (SR / NFFT)) + 1
    B = P[:, :nb]
    r = np.fft.irfft(B, 2 * (nb - 1))[:, :order + 1]
    r[:, 0] *= 1.0 + 1e-6                              # white-noise correction (-60 dB floor)
    # lag window: a 60 Hz Gaussian in frequency, keeps poles from collapsing on single harmonics of high voices
    fs = 2 * (nb - 1) * SR / NFFT
    lag = np.arange(order + 1)
    r = r * np.exp(-0.5 * (2 * np.pi * 60.0 * lag / fs) ** 2)[None, :]
    T = len(P)
    a = np.zeros((T, order + 1))
    a[:, 0] = 1.0
    err = r[:, 0].copy()
    for i in range(1, order + 1):
        acc = r[:, i] + (a[:, 1:i] * r[:, i - 1:0:-1]).sum(1)
        k = -acc / np.maximum(err, 1e-12)
        prev = a.copy()
        a[:, i] = k
        a[:, 1:i] = prev[:, 1:i] + k[:, None] * prev[:, i - 1:0:-1]
        err = err * (1 - k * k)
    return a, fs


def formant_candidates(a, fs):
    """Roots of each predictor polynomial (eigenvalues of the companion matrix): resonance frequency and
    bandwidth. Kept: 90 Hz < f < ceil-50, bandwidth < 600 Hz (wider poles shape the spectral tilt, not formants)."""
    out = []
    for row in a:
        z = np.roots(row)
        z = z[np.imag(z) > 0]
        f = np.angle(z) * fs / (2 * np.pi)
        bw = -np.log(np.abs(z) + 1e-12) * fs / np.pi
        keep = (f > 90) & (f < fs / 2 - 50) & (bw < 600)
        o = np.argsort(f[keep])
        out.append((f[keep][o], bw[keep][o]))
    return out


# Formant ranges for any adult or child voice, from the acoustic-phonetics literature (Peterson & Barney 1952
# extremes widened by the tube scaling of a 12 cm child tract): a candidate outside its range cannot be that formant.
F_RANGE = [(180.0, 1300.0), (550.0, 3200.0), (1500.0, 4300.0), (2600.0, 5200.0)]


def track_formants(cands, voiced, L=L_REF):
    """F1..F4 per frame by dynamic programming over the candidate assignments (a Viterbi, not a search over
    weights). F1 and F2 are required (they carry the vowel); F3 and F4 may be missing, which happens in back
    vowels and in high voices, where F3 is weak. Cost of an assignment: F1, F2 weakly pulled to the neutral tube
    of length L (Bark), bandwidths, candidates skipped below F2 (a skipped resonance there is almost always a
    real formant), a missing F3/F4, and the jump from the previous frame. Unvoiced frames get NaN."""
    T = len(cands)
    neutral = np.array([(2 * k - 1) * SOUND_CM_S / (4 * L) for k in range(1, 5)])
    zn = bark(neutral)
    out = np.full((T, 4), np.nan)
    bws = np.full((T, 4), np.nan)
    states = []
    for t in range(T):
        f, bw = cands[t]
        opts = []
        n = len(f)
        if voiced[t] and n >= 2:
            ok = [[F_RANGE[q][0] <= f[i] <= F_RANGE[q][1] for i in range(n)] for q in range(4)]
            for i in range(n):
                if not ok[0][i]:
                    continue
                for j in range(i + 1, n):
                    if not ok[1][j]:
                        continue
                    # very weak pull to the neutral tube (vowels move F1, F2 by 4-5 Bark from it)
                    base = 0.03 * ((bark(f[i]) - zn[0]) ** 2 + (bark(f[j]) - zn[1]) ** 2)
                    base += (bw[i] + bw[j]) / 300.0
                    # a narrow resonance skipped below F2 is almost always a real formant; a wide one may be
                    # the glottal pole or a nasal zero neighbour
                    base += sum(2.0 if bw[q] < 300 else 0.3 for q in range(j) if q != i)
                    for k in [None] + [k for k in range(j + 1, n) if ok[2][k]]:
                        for l in [None] + ([l for l in range(k + 1, n) if ok[3][l]] if k is not None else []):
                            c = base
                            c += 1.0 if k is None else bw[k] / 300.0
                            c += 0.3 if l is None else bw[l] / 300.0
                            # the same between F2 and the highest formant taken: a narrow resonance there is F3
                            # (session speaker: F3 at 2750 Hz, bandwidth 185, skipped for F4 at 3850)
                            top = l if l is not None else k
                            if top is not None:
                                c += sum(2.0 if bw[q] < 300 else 0.3 for q in range(j + 1, top) if q != k)
                            fv = [f[i], f[j], f[k] if k is not None else np.nan, f[l] if l is not None else np.nan]
                            bv = [bw[i], bw[j], bw[k] if k is not None else np.nan, bw[l] if l is not None else np.nan]
                            opts.append((c, fv, bv))
        states.append(opts)
    t = 0
    while t < T:
        if not states[t]:
            t += 1
            continue
        s = t
        while t < T and states[t]:
            t += 1
        cost = [np.array([o[0] for o in states[s]])]
        back = [None]
        for u in range(s + 1, t):
            prev = bark(np.array([o[1][:3] for o in states[u - 1]]))
            cur = bark(np.array([o[1][:3] for o in states[u]]))
            d = (cur[:, None, :] - prev[None, :, :]) ** 2
            # F3 jumps count when both frames have it; one frame with F3 and one without costs a little
            d[..., 2] = np.where(np.isfinite(d[..., 2]), d[..., 2], 0.5)
            jump = d.sum(2)
            tot = cost[-1][None, :] + jump
            bk = tot.argmin(1)
            cost.append(tot[np.arange(len(cur)), bk] + np.array([o[0] for o in states[u]]))
            back.append(bk)
        j = int(cost[-1].argmin())
        for q in range(len(cost) - 1, -1, -1):
            u = s + q
            out[u] = states[u][j][1]
            bws[u] = states[u][j][2]
            if back[q] is not None:
                j = int(back[q][j])
    return out, bws


def bark(f):
    """Traunmueller (1990) critical-band rate."""
    return 26.81 * f / (1960.0 + f) - 0.53


def tract_length(F):
    """Speaker tract length by formant dispersion (Fitch 1997; Reby & McComb 2003): in a uniform tube
    Fk = (2k-1) dF / 2 with dF = c / 2L, so dF is the least-squares slope of the measured Fk on (2k-1)/2.
    Per frame with at least F1-F3, then the median over frames. All formants enter; the high ones weigh most,
    and they move least with the vowel. Clamped to 10..20 cm (small child .. tall man)."""
    est = []
    for row in F:
        k = np.where(np.isfinite(row))[0]
        if len(k) < 3 or 2 not in k:
            continue
        x = (2 * (k + 1) - 1) / 2.0
        dF = float((row[k] * x).sum() / (x * x).sum())
        est.append(SOUND_CM_S / (2 * dF))
    if not est:
        return L_REF
    return float(np.clip(np.median(est), 10.0, 20.0))


class Ear:
    """All the per-frame features of one utterance."""

    def __init__(self, x):
        self.x = x
        self.P = spectrum(x)
        self.T = len(self.P)
        self.mfcc, self.logmel = mfcc(self.P)
        self.energy = log_energy(x)[:self.T]
        self.per, self.f0 = periodicity(x)
        # noise floor: the quietest 10% of frames (the start of a capture is often already speech in tests),
        # never below -75 dB (16-bit digital silence would make every breath look loud)
        self.floor = max(float(np.percentile(self.energy, 10)), -75.0)
        self.peak = float(self.energy.max())
        # voiced: periodic, clearly above the floor, and within 35 dB of the loudest frame (vowels and voiced
        # consonants of one utterance stay inside that range; bursts and breath noise fall below it)
        self.voiced = (self.per > 0.45) & (self.energy > self.floor + 12) & (self.energy > self.peak - 35)
        self.f0 = np.where(self.voiced, self.f0, np.nan)
        # pass 1: reference ceiling; pass 2: the ceiling that holds 5 formants of this speaker's tube,
        # 5 * c / 2L (5000 Hz for 17.5 cm, ~6000 for 14.5 cm), and the speaker's neutral tube as weak prior
        self.L = L_REF
        for ceil in (CEIL, None):
            if ceil is None:
                # L limited to 13..18.5 cm for this: a wrong L from pass 1 must not move the ceiling so far
                # that F1 and F2 merge (too high) or F4 falls out (too low)
                ceil = 5 * SOUND_CM_S / (2 * min(max(self.L, 13.0), 18.5))
            a, fs = lpc_from_spectrum(self.P, ceil=ceil)
            self.cands = formant_candidates(a, fs)
            self.F, self.bw = track_formants(self.cands, self.voiced, self.L)
            self.L = tract_length(self.F)
        self.ceil = ceil
        self.alpha = self.L / L_REF                    # f' = f * L / 17.5
        self.Fn = self.F * self.alpha                  # formants scaled to the reference speaker
        # spectral centroid (Hz) of 0..8 kHz per frame, used by the consonant layers
        self.centroid = (self.P * FREQ).sum(1) / (self.P.sum(1) + 1e-12)


def main():
    for path in sys.argv[1:]:
        e = Ear(load(path))
        print(f'{os.path.basename(path)}: {e.T} frames, floor {e.floor:.1f} dB, ceil {e.ceil:.0f}, L {e.L:.1f} cm, alpha {e.alpha:.3f}')
        for t in range(e.T):
            f = e.F[t]
            print(f'{t:4d} {e.energy[t]:6.1f} {e.per[t]:4.2f} {"V" if e.voiced[t] else "."} '
                  f'{e.f0[t]:6.1f} {f[0]:6.0f} {f[1]:6.0f} {f[2]:6.0f} {f[3]:6.0f}  c{e.centroid[t]:5.0f}')


if __name__ == '__main__':
    main()
