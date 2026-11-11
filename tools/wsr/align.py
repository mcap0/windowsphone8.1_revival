"""wsr align: forced alignment of a known sentence to its recording (Viterbi on a left-to-right chain of phones).

Emissions are hand-written from the ear's features, per broad class; the vowels use the vowel detector's scores.
A frame's cost for each phone class:
  silence     energy close to the floor
  vowel v     voiced, loud (within ~15 dB of the take's peak), and the detector's score for v
  voiced cons voiced, less loud than a vowel (nasals, liquids, glides, b d g v z dz dZ)
  unvoiced    not voiced: fricatives (energy above the floor, high centroid for s S), stops and affricates
              (closure silence or burst)
Durations: minimum frames per phone (vowel 3, stressed 4; consonant 2, stop 3; long x1.8), by a chain of states.
Between words an optional pause. The alignment is used to measure (and, for the Lumia, to fit: see MISURE.md),
never as a decoder.

    python3 tools/wsr/align.py file.wav "testo"     prints the segments
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear  # noqa: E402
import g2p  # noqa: E402
import vowels as V  # noqa: E402

VSYM = {'a': ['a'], 'e': ['e'], 'E': ['ɛ'], 'i': ['i'], 'o': ['o'], 'O': ['ɔ'], 'u': ['u'],
        'e*': ['e', 'ɛ'], 'o*': ['o', 'ɔ']}
STOP_U, STOP_V = set('ptk'), set('bdg')
FRIC_U, FRIC_V = {'f', 's', 'S'}, {'v', 'z'}
AFFR_U, AFFR_V = {'ts', 'tS'}, {'dz', 'dZ'}
SON = {'m', 'n', 'J', 'l', 'L', 'r', 'j', 'w'}


def sequence(text):
    """[(symbol, long, stressed)] with '#' between words (optional pause) and '_' at both ends."""
    seq = [('_', False, False)]
    for k, w in enumerate(g2p.words(text)):
        if k:
            seq.append(('#', False, False))
        seq += g2p.word_phones(w)
    seq.append(('_', False, False))
    return seq


def min_frames(sym, long, stressed):
    if sym in ('_', '#'):
        return 3 if sym == '_' else 2
    if sym in VSYM:
        m = 4 if stressed else 3
    elif sym in STOP_U | STOP_V | AFFR_U | AFFR_V:
        m = 3
    else:
        m = 2
    return int(math.ceil(m * (1.8 if long else 1.0)))


def emissions(e, vsc):
    """Frame x class log-scores. Classes: sil, unv_fric, unv_stop, voiced_cons, and one per vowel."""
    E = e.energy
    rel = E - e.peak                       # dB below the take's peak
    above = E - e.floor
    v = e.voiced.astype(float)
    hi = np.clip((e.centroid - 2500.0) / 1500.0, -1, 1)
    out = {}
    out['sil'] = -np.clip(above - 10, 0, None) / 3.0 - 3.0 * v
    out['unv_fric'] = -4.0 * v - np.clip(12 - above, 0, None) / 3.0 + 1.0 * hi
    out['unv_stop'] = -4.0 * v - np.clip(rel + 10, 0, None) / 6.0           # closure (quiet) or short burst
    out['voiced_cons'] = -4.0 * (1 - v) - np.clip(rel + 6, 0, None) / 3.0 - np.clip(-30 - rel, 0, None) / 4.0
    base = -4.0 * (1 - v) - np.clip(-15 - rel, 0, None) / 3.0
    best = np.where(np.isfinite(vsc), vsc, -1e9).max(1, keepdims=True)
    rv = np.where(np.isfinite(vsc), np.clip(vsc - best, -8, 0), -8.0)    # relative vowel score, 0 = best
    for k, name in enumerate(V.VOWELS):
        out['v:' + name] = base + rv[:, k]
    return out


def phone_class(sym):
    if sym in ('_', '#'):
        return ['sil']
    if sym in VSYM:
        return ['v:' + x for x in VSYM[sym]]
    if sym in FRIC_U:
        return ['unv_fric']
    if sym in STOP_U or sym in AFFR_U:
        return ['unv_stop', 'unv_fric']
    if sym in STOP_V or sym in AFFR_V:
        return ['voiced_cons', 'unv_stop']
    return ['voiced_cons']                 # sonorants, v z


def align(e, vsc, text):
    """[(symbol, long, stressed, start, end)] frames, or None if the take is shorter than the sentence."""
    seq = sequence(text)
    em = emissions(e, vsc)
    cls = [np.max(np.stack([em[c] for c in phone_class(s)]), 0) for s, _, _ in seq]
    # states: each phone expanded to its minimum length; last state loops
    states, owner = [], []
    for k, (s, l, st) in enumerate(seq):
        for j in range(min_frames(s, l, st)):
            states.append(k)
            owner.append(j == min_frames(s, l, st) - 1)
    S, T = len(states), e.T
    if T < S - sum(1 for s in seq if s[0] == '#') * 2:
        return None
    NEG = -1e18
    cost = np.full(S, NEG)
    cost[0] = cls[0][0]
    back = np.zeros((T, S), dtype=np.int32)
    # allowed predecessors: same state (if loop), previous state, and skipping a whole '#' pause
    first = {}
    for i, k in enumerate(states):
        first.setdefault(k, i)
    skip_from = np.full(S, -1)
    for k, (s, _, _) in enumerate(seq):
        if s == '#' and k + 1 < len(seq):
            # first state of phone k+1 may come from the last state of phone k-1
            skip_from[first[k + 1]] = first[k] - 1
    loop = np.array(owner)
    emis = np.stack([cls[k] for k in states], 1)          # T x S
    for t in range(1, T):
        stay = np.where(loop, cost, NEG)
        move = np.concatenate([[NEG], cost[:-1]])
        sk = np.where(skip_from >= 0, cost[np.maximum(skip_from, 0)], NEG)
        cand = np.stack([stay, move, sk])
        b = cand.argmax(0)
        cost = cand.max(0) + emis[t]
        back[t] = np.where(b == 0, np.arange(S), np.where(b == 1, np.arange(S) - 1, skip_from))
    i = S - 1
    path = np.zeros(T, dtype=np.int32)
    for t in range(T - 1, -1, -1):
        path[t] = i
        i = back[t, i]
    segs, t = [], 0
    while t < T:
        k = states[path[t]]
        a = t
        while t < T and states[path[t]] == k:
            t += 1
        s, l, st = seq[k]
        segs.append((s, l, st, a, t))
    return segs


def main():
    path, text = sys.argv[1], sys.argv[2]
    e = ear.Ear(ear.load(path))
    vsc = V.frame_scores(e)
    for s, l, st, a, b in align(e, vsc, text):
        if s in ('_', '#'):
            if s == '_' or b - a > 0:
                print(f'{a * 10:6d}-{b * 10:6d} ms  {s}')
            continue
        print(f'{a * 10:6d}-{b * 10:6d} ms  {s}{":" if l else ""}{"\'" if st else ""}')


if __name__ == '__main__':
    main()
