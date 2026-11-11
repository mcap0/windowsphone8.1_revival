"""Rewrite Kaldi / OpenFst / vosk-api for a build without C++ exceptions (LLVM has no C++ EH for Windows ARM32:
"Cannot select: cleanupret"). In place in vosk/deps/src (git clones: `git diff` shows the change; run again is a no-op).

  try {                      ->  {
  catch (...)                ->  if (false)
  catch (T &name)            ->  if (false) for (T &name = ::wp81::never(); false;)   (the dead branch still compiles)
  catch (T &)                ->  if (false)
  throw X;                   ->  ::wp81::fatal(X);     (logs the message, then aborts: see vosk/wp81-vosk.h)
  throw;                     ->  ::wp81::fatal();
  throw()  (exception spec)  ->  noexcept

    python3 vosk/noexcept.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'deps', 'src')
DIRS = ['openfst/src', 'kaldi/src', 'vosk-api/src']

CATCH = re.compile(r'\bcatch\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)')
SPEC = re.compile(r'\bthrow\s*\(\s*\)')
THROW = re.compile(r'\bthrow\b(\s*;|\s+[^;]*;|\s*\([^;]*;|\s*[A-Za-z_:][^;]*;)')


def fix_catch(m):
    decl = m.group(1).strip()
    if decl == '...':
        return 'if (false)'
    # a name at the end of the declaration?
    nm = re.match(r'^(.*?[\s&*])([A-Za-z_]\w*)$', decl)
    if nm and nm.group(1).strip() and not nm.group(1).strip().endswith('::'):
        typ = nm.group(1).strip()
        if '&' not in typ and '*' not in typ:
            typ += ' &'
        return f'if (false) for ({typ} {nm.group(2)} = ::wp81::never(); false;)'
    return 'if (false)'


def fix_throw(m):
    rest = m.group(1).strip()
    if rest == ';':
        return '::wp81::fatal();'
    return f'::wp81::fatal({rest[:-1].strip()});'


def rewrite(text):
    # strings and comments are replaced by placeholders, the code is rewritten, then they are put back
    seg = re.compile(r'("(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/)', re.S)
    saved = []

    def keep(m):
        saved.append(m.group(0))
        return f'\x00{len(saved) - 1}\x00'
    p = seg.sub(keep, text)
    p = SPEC.sub('noexcept', p)
    p = re.sub(r'\btry\s*\{', '{', p)
    p = CATCH.sub(fix_catch, p)
    p = THROW.sub(fix_throw, p)
    return re.sub('\x00(\\d+)\x00', lambda m: saved[int(m.group(1))], p)


def main():
    changed = 0
    for d in DIRS:
        for root, _, files in os.walk(os.path.join(SRC, d)):
            for f in files:
                if not f.endswith(('.cc', '.h', '.cpp')):
                    continue
                path = os.path.join(root, f)
                old = open(path, encoding='utf-8', errors='surrogateescape').read()
                if 'wp81::' in old and 'noexcept.py' in old:
                    continue
                new = rewrite(old)
                if new != old:
                    open(path, 'w', encoding='utf-8', errors='surrogateescape').write(new)
                    changed += 1
    print(changed, 'files rewritten')


if __name__ == '__main__':
    main()
