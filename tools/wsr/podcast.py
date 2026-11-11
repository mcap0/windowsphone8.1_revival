"""A YouTube video played from the PC speakers and recorded by the Lumia ("registra lungo <nome>") -> takes with
transcripts, in the same format as the reading sessions (share/voce/<session>-frasi/NNN.wav + elenco.tsv).

1. The original audio (share/yt/<id>.16k.wav) and the automatic Italian captions with word times (<id>.it.vtt).
2. Sync: each 30 s piece of the phone recording is found in the original by cross-correlating the two log-energy
   envelopes (10 ms frames, speech-normalised); pieces are consecutive, so each one is searched near the previous
   one's place (the phone's and the PC's clocks may drift a little).
3. Phrases: caption words grouped up to a pause of 250 ms or 10 words, then cut from the phone recording with 150 ms
   margins. Automatic captions have some wrong words: the labels come from dtwalign, which tolerates a few.

    python3 tools/wsr/podcast.py <session> <video id>
"""
import os
import re
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ear  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'share')


def ts(s):
    h, m, x = s.split(':')
    return int(h) * 3600 + int(m) * 60 + float(x)


def caption_words(path):
    """[(start s, end s, word)] from a YouTube automatic-caption vtt. Each cue repeats the previous line and adds a
    new one; the new line has <c> word times, or none (then its words are spread evenly over the cue). Cues of
    10 ms only repeat text and are skipped."""
    cues, cur = [], None
    for line in open(path, encoding='utf-8'):
        m = re.match(r'(\d+:\d+:\d+\.\d+) --> (\d+:\d+:\d+\.\d+)', line)
        if m:
            cur = [ts(m.group(1)), ts(m.group(2)), []]
            cues.append(cur)
        elif cur is not None and line.strip():
            cur[2].append(line.strip())
    words, last = [], ''
    for a, b, lines in cues:
        if b - a < 0.05 or not lines:
            continue
        new = lines[-1]
        plain = re.sub(r'<[^>]+>', '', new).strip()
        if not plain or plain == last:
            continue
        last = plain
        if '<c>' in new:
            parts = re.split(r'<(\d+:\d+:\d+\.\d+)>', new)
            t = a
            first = re.sub(r'</?c>', '', parts[0]).strip()
            seq = [(a, first)] if first else []
            for k in range(1, len(parts) - 1, 2):
                w = re.sub(r'</?c>', '', parts[k + 1]).strip()
                if w:
                    seq.append((ts(parts[k]), w))
        else:
            ws = plain.split()
            seq = [(a + (b - a) * k / len(ws), w) for k, w in enumerate(ws)]
        for k, (t, w) in enumerate(seq):
            end = seq[k + 1][0] if k + 1 < len(seq) else b
            for piece in w.split():
                words.append((t, end, piece))
    return words


def envelope(x):
    e = ear.log_energy(x)
    sp = e > max(np.percentile(e, 30), -70.0)
    z = (e - e[sp].mean()) / (e[sp].std() + 1e-6)
    # silence is 0, not -1: silence against silence must not count as a match (it pinned every piece to the
    # edge of the video's 20 silent minutes)
    return np.where(e > -70.0, z, 0.0)


def find(piece_env, orig_env, lo, hi):
    """Best offset (frames) of piece in orig within [lo, hi], and its normalised correlation."""
    lo, hi = max(0, lo), min(len(orig_env) - len(piece_env), hi)
    seg = orig_env[lo:hi + len(piece_env)]
    n = len(seg) + len(piece_env)
    f = 1 << int(np.ceil(np.log2(n)))
    c = np.fft.irfft(np.fft.rfft(seg, f) * np.conj(np.fft.rfft(piece_env, f)), f)[:hi - lo + 1]
    k = int(c.argmax())
    return lo + k, float(c[k] / len(piece_env))


def write_wav(path, x):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(ear.SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())


def main():
    session, vid = sys.argv[1], sys.argv[2]
    d = os.path.join(ROOT, 'voce', session)
    pieces = sorted(p for p in os.listdir(d) if p.endswith('.wav'))
    orig = ear.load(os.path.join(ROOT, 'yt', vid + '.16k.wav'))
    oenv = envelope(orig)
    words = caption_words(os.path.join(ROOT, 'yt', vid + '.it.vtt'))
    # phone recording, piece by piece, mapped to original time
    placed = []           # (phone samples, original start s)
    prev = None
    for p in pieces:
        x = ear.load(os.path.join(d, p))
        penv = envelope(x)
        if prev is None:
            # only where the original has speech (this video is silent from 14 to 34 min): a quiet stretch
            # correlates with anything
            off, c = find(penv, oenv, 0, len(oenv))
        else:
            want = prev + int(30 * 100)
            off, c = find(penv, oenv, want - 300, want + 300)
        print(f'{p}: original {off / 100:8.2f} s, correlation {c:.2f}')
        placed.append((x, off / 100.0, c))
        prev = off
    # phrases
    out = os.path.join(ROOT, 'voce', session + '-frasi')
    os.makedirs(out, exist_ok=True)
    rows, n = [], 0
    for x, start, c in placed:
        if c < 0.3:
            continue
        end = start + len(x) / ear.SR
        ws = [w for w in words if w[0] >= start + 0.15 and w[1] <= end - 0.15]
        group = []
        for w in ws + [None]:
            if group and (w is None or w[0] - group[-1][1] > 0.25 or len(group) >= 10):
                a = max(0, int((group[0][0] - start - 0.15) * ear.SR))
                b = min(len(x), int((group[-1][1] - start + 0.15) * ear.SR))
                text = ' '.join(g[2] for g in group)
                if b - a > 0.6 * ear.SR and re.search(r'[a-zàèéìòù]', text.lower()):
                    n += 1
                    name = f'{n:03d}.wav'
                    write_wav(os.path.join(out, name), x[a:b])
                    rows.append(f'{name}\t{text}\t{(b - a) * 1000 // ear.SR}\tpodcast\tlumia')
                group = []
            if w is not None:
                group.append(w)
    with open(os.path.join(out, 'elenco.tsv'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(rows) + '\n')
    print(f'{n} phrases -> {os.path.normpath(out)}')


if __name__ == '__main__':
    main()
