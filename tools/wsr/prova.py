"""The test round (user's idea, 2026-11-03): fresh grammar sentences -> read on the phone -> decoded on the PC ->
an error report to adjust the weights by hand.

    python3 tools/wsr/prova.py nuovo [n]   writes share/prova.txt (n new grammar sentences, a new seed each round)
        on the phone: "registra prova" (fetches share/prova.txt from the inbox server)
    python3 tools/wsr/prova.py esito       decodes the latest prova session -> share/prova-esito.txt

Every round is a set never seen before: it is measured first and only then used to change weights (MISURE.md).
The decoder's candidates are its usual sentences plus this round's (the full-grammar decoder is still to come).
"""
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'wsp'))
ROOT = os.path.join(HERE, '..', '..', 'share')
BATCH = os.path.join(ROOT, 'prova.txt')
ROUNDS = os.path.join(ROOT, 'prova-giri.txt')


def nuovo(n):
    import grammar
    done = set()
    k = 0
    if os.path.exists(ROUNDS):
        for line in open(ROUNDS, encoding='utf-8'):
            k += 1
    for f in glob.glob(os.path.join(ROOT, 'voce', '*', 'elenco.tsv')):
        for line in open(f, encoding='utf-8'):
            p = line.split('\t')
            if len(p) > 1:
                done.add(' '.join(p[1].lower().split()))
    seed = 1000 + k
    out, seen = [], set()
    for t, intent in grammar.samples(n * 6, seed=seed):
        key = ' '.join(t.lower().split())
        if key in seen or key in done or len(key.split()) < 2:
            continue
        seen.add(key)
        out.append(t)
        if len(out) >= n:
            break
    with open(BATCH, 'w', encoding='utf-8', newline='\n') as f:
        for i, t in enumerate(out, 1):
            f.write(f'prova-{i:03d}\t{t}\n')
    with open(ROUNDS, 'a', encoding='utf-8') as f:
        f.write(f'round {k + 1}\tseed {seed}\t{len(out)} sentences\n')
    print(f'round {k + 1}: {len(out)} sentences -> {os.path.normpath(BATCH)}')


def esito():
    import decode
    import dtwalign
    import ear
    import fit
    import mfccmodel as M
    import vowels as V
    sessions = sorted(glob.glob(os.path.join(ROOT, 'voce', '*-prova*')))
    if not sessions:
        print('no prova session yet')
        return
    d = sessions[-1]
    items = []
    for line in open(os.path.join(d, 'elenco.tsv'), encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 2:
            items.append((os.path.join(d, p[0]), p[1]))
    tpls = decode.load_templates()
    extra = [(t, decode.template(t)) for _, t in items]
    tpls += [(t, F) for t, F in extra if F is not None and len(F) > 5]
    model = M.Model()
    lines, ok, vtot, vok = [], 0, 0, 0
    for path, said in items:
        x = ear.load(path)
        sc = decode.rescore(x, decode.decode(x, tpls), model)
        names = [' '.join(decode.norm(t)) for _, t in sc]
        want = ' '.join(decode.norm(said))
        r = names.index(want) + 1 if want in names else -1
        ok += r == 1
        # vowels: aligned on the true sentence, the model's 5-way choice per vowel segment
        errs = []
        got = dtwalign.align(x, want)
        if got is not None:
            segs, e = got
            L = model.ll(M.features(e))
            for ph, a, b in segs:
                v = fit.LAB.get(ph)
                if v is None or b - a < 5:
                    continue
                fr = list(range(a + (b - a) // 4, b - (b - a) // 4))
                S = L[fr].sum(0) + model.logprior
                g = model.vowels[int(S.argmax())]
                vtot += 1
                if V.FIVE[g] == V.FIVE[v]:
                    vok += 1
                else:
                    errs.append(f'{V.FIVE[v]}->{V.FIVE[g]}@{a * 10}ms')
        lines.append(f'{os.path.basename(path)}\t{"OK" if r == 1 else "NO"}\trank {r}\t"{said}"\t-> "{sc[0][1]}"\t'
                     + ' '.join(errs))
    head = (f'{os.path.basename(d)}: right sentence first {ok}/{len(items)}; vowel segments 5-way '
            f'{100 * vok / max(vtot, 1):.1f}% ({vok}/{vtot}); {len(tpls)} candidates')
    out = os.path.join(ROOT, 'prova-esito.txt')
    with open(out, 'w', encoding='utf-8') as f:
        f.write(head + '\n' + '\n'.join(lines) + '\n')
    print(head)
    print('report:', os.path.normpath(out))


if __name__ == '__main__':
    if sys.argv[1] == 'nuovo':
        nuovo(int(sys.argv[2]) if len(sys.argv) > 2 else 40)
    else:
        esito()
