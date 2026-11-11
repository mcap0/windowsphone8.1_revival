"""wsr export: the phone's recognizer data -> apps/Assistente/Assistente/Assets/voce.wsr (read by Core/Wsr.cpp).

Little-endian:
  'WSR1', u32 sentences, u32 D (12)
  vowel model: u32 V; f32 means V x 12; f32 widths 12; f32 log priors V; u8 five-group V (i e a o u = 0..4)
  per sentence: u16 utf-8 length, bytes; u16 T; i16 T x D (x 1024): the DTW template (dtwalign.feats of MBROLA it3
  at 150 wpm, speech span only); u16 vowel segments, each u8 vowel, u16 start, u16 end (template frames)

    python3 tools/wsr/export.py
"""
import os
import struct
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import decode  # noqa: E402
import dtwalign  # noqa: E402
import fit  # noqa: E402
import mfccmodel as M  # noqa: E402
import vowels as V  # noqa: E402

OUT = os.path.join(HERE, '..', '..', 'apps', 'Assistente', 'Assistente', 'Assets', 'voce.wsr')
GROUP = {'i': 0, 'e': 1, 'a': 2, 'o': 3, 'u': 4}


def one(text):
    t = ' '.join(decode.norm(text))
    with tempfile.TemporaryDirectory() as d:
        got = V.synth('mb-it3', t, 150, 1.0, d)
    if got is None:
        return None
    y, segs = got
    e, F = dtwalign.feats(y)
    a, b = dtwalign.speech_span(e)
    vs = []
    for ph, s, u in segs:
        v = fit.LAB.get(ph)
        if v is None:
            continue
        ja, jb = int(s / 10) - a, int(u / 10) - a
        if jb - ja >= 1 and ja >= 0 and jb <= b - a:
            vs.append((v, ja, jb))
    return t, F[a:b], vs


def main():
    import multiprocessing
    texts = decode.candidates()
    with multiprocessing.Pool() as pool:
        got = [g for g in pool.map(one, texts) if g is not None and len(g[1]) > 5]
    m = M.Model()
    out = bytearray(b'WSR1')
    out += struct.pack('<II', len(got), 12)
    out += struct.pack('<I', len(m.vowels))
    out += np.asarray(m.mu[:, :12], '<f4').tobytes()
    out += np.asarray(m.sd[0, :12], '<f4').tobytes()
    out += np.asarray(m.logprior, '<f4').tobytes()
    out += bytes(GROUP[V.FIVE[v]] for v in m.vowels)
    for t, F, vs in got:
        b = t.encode('utf-8')
        out += struct.pack('<H', len(b)) + b
        out += struct.pack('<H', len(F))
        out += np.clip(np.round(F * 1024), -32767, 32767).astype('<i2').tobytes()
        out += struct.pack('<H', len(vs))
        for v, ja, jb in vs:
            out += struct.pack('<BHH', m.vowels.index(v), ja, jb)
    open(OUT, 'wb').write(bytes(out))
    print(f'{len(got)} sentences, {len(out)} bytes -> {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
