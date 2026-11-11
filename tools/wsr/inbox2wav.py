"""Recordings from Assistente's recording mode ("registra [nome]") -> WAV files for the wsr measurements.

The phone posts each take to tools/inbox, which appends it to share/inbox/voce.txt as
    ===== <time> =====
    session <TAB> NNN.wav <TAB> transcript <TAB> ms <TAB> capture format <TAB> microphone <TAB> base64 WAV
This writes share/voce/<session>/NNN.wav and share/voce/<session>/elenco.tsv (file, transcript, ms, format, mic).
Takes already written are left alone, so it can run again after every session.

    python3 tools/wsr/inbox2wav.py
"""
import base64
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'share')
INBOX = os.path.join(ROOT, 'inbox', 'voce.txt')
OUT = os.path.join(ROOT, 'voce')


def main():
    if not os.path.exists(INBOX):
        print('nothing yet:', os.path.normpath(INBOX))
        return
    new = 0
    seen = {}
    for line in open(INBOX, encoding='utf-8', errors='replace'):
        p = line.rstrip('\r\n').split('\t')
        if len(p) < 7 or not p[1].endswith('.wav'):
            continue
        session, name, say, ms, fmt, mic, b64 = p[:7]
        if not session.replace('-', '').isalnum() or not name[:-4].isdigit():
            continue
        d = os.path.join(OUT, session)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, name)
        seen.setdefault(session, {})[name] = (say, ms, fmt, mic)
        if not os.path.exists(path):
            with open(path, 'wb') as f:
                f.write(base64.b64decode(b64))
            new += 1
    for session, takes in seen.items():
        with open(os.path.join(OUT, session, 'elenco.tsv'), 'w', encoding='utf-8') as f:
            for name in sorted(takes):
                f.write('\t'.join((name,) + takes[name]) + '\n')
        print(f'{session}: {len(takes)} takes')
    print(f'{new} new WAV files in', os.path.normpath(OUT))


if __name__ == '__main__':
    sys.exit(main())
