"""The agreed reading texts (tools/wsr/testi/*.txt) -> one sentence per line in Assistente's Assets/testo.txt,
for "registra testo [nome]". Sentences end at . ! ? ; and long ones are cut at : or , (about 15 words at most),
so each take is short and its transcript is exactly the shown line.

    python3 tools/wsr/testo.py
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'apps', 'Assistente', 'Assistente', 'Assets', 'testo.txt')
ORDER = ['tramontana', 'pinocchio1']
MAXW = 15


def pieces(text):
    for s in re.split(r'(?<=[.!?;])\s+', text.strip()):
        words = s.split()
        if len(words) <= MAXW:
            yield s
            continue
        cur = []
        for w in words:
            cur.append(w)
            if len(cur) >= 6 and w[-1] in ',:' and len(cur) <= MAXW:
                yield ' '.join(cur)
                cur = []
            elif len(cur) > MAXW + 5:
                yield ' '.join(cur)
                cur = []
        if cur:
            yield ' '.join(cur)


def main():
    lines = []
    for name in ORDER:
        text = open(os.path.join(HERE, 'testi', name + '.txt'), encoding='utf-8').read()
        for k, p in enumerate(pieces(text), 1):
            lines.append(f'{name}-{k:03d}\t{p}')
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(len(lines), 'lines ->', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
