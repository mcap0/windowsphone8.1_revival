"""Check align.py on MBROLA speech, where the true phone times are known (espeak --pho): for each vowel nucleus,
in order, is the centre of our aligned vowel inside the true vowel segment? Sentences: the reading text lines.

    python3 tools/wsr/aligncheck.py [n_lines]
"""
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import align  # noqa: E402
import ear  # noqa: E402
import vowels as V  # noqa: E402

TEXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'apps', 'Assistente', 'Assistente',
                    'Assets', 'testo.txt')
PV = set('a a1 e e1 E E1 i i1 o o1 O O1 u u1'.split())


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    lines = [l.split('\t')[1].strip() for l in open(TEXT, encoding='utf-8')][:n]
    tot = inside = skipped = 0
    with tempfile.TemporaryDirectory() as d:
        for voice in ('mb-it3', 'mb-it4'):
            for text in lines:
                got = V.synth(voice, text, 160, 1.0, d)
                if got is None:
                    skipped += 1
                    continue
                x, segs = got
                tv = [(a, b) for ph, a, b in segs if ph in PV]
                e = ear.Ear(x)
                al = align.align(e, V.frame_scores(e), text)
                if al is None:
                    skipped += 1
                    continue
                ov = [(a, b) for s, l, st, a, b in al if s in align.VSYM]
                if len(ov) != len(tv):
                    skipped += 1
                    continue
                for (a, b), (ta, tb) in zip(ov, tv):
                    c = (a + b) / 2 * 10 + 12.5
                    tot += 1
                    inside += ta <= c <= tb
    print(f'vowel centres inside the true vowel: {inside}/{tot} = {100 * inside / max(tot, 1):.1f}%  '
          f'(sentences skipped: {skipped}, vowel count differs from espeak or mbrola refused)')


if __name__ == '__main__':
    main()
