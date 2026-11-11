"""wsr g2p: Italian spelling -> phonemes, hand-written rules (VOCE-PIANO.md 4.3).

Phones (ASCII): vowels a e E i o O u, plus e* o* = a stressed mid vowel whose open/closed quality the spelling does
not say (scored as the better of the two). Consonants: p b t d k g f v s z S ts dz tS dZ m n J l L r j w.
A phone is (symbol, long, stressed): long = geminate (written double, or ʃ ɲ ʎ ts dz between vowels, which are
always long in Italian).

Rules: c/g soft before e i (the i of ci/gi/sci + vowel is only spelling); ch gh hard; gli -> ʎ; gn -> ɲ; sc+e/i -> ʃ;
qu -> kw; h silent; s voiced between vowels and before a voiced consonant; z -> ts except a short list (dz);
unstressed i/u before a vowel -> j/w; stress from the accent mark, else the penultimate syllable, else a small
lexicon of proparoxytones (words stressed on the third-to-last syllable) that occur in our texts and commands.
Unstressed e/o are closed (standard Italian).

    python3 tools/wsr/g2p.py --test       hand-written expectations
    python3 tools/wsr/g2p.py parola ...   prints the transcription
"""
import re
import sys
import unicodedata

VOWEL_LETTERS = 'aeiou'
VOWELSET = set(VOWEL_LETTERS)          # a set: '' is not a vowel ('' in 'aeiou' is True)
ACCENT = {'à': ('a', 'a'), 'è': ('e', 'E'), 'é': ('e', 'e'), 'ì': ('i', 'i'), 'í': ('i', 'i'), 'ò': ('o', 'O'),
          'ó': ('o', 'o'), 'ù': ('u', 'u'), 'ú': ('u', 'u')}
VOICED = set('bdgvmnlr')
DZ_WORDS = {'zero', 'zona', 'zanzara', 'zaino', 'zeta', 'zebra', 'zelo', 'zinco', 'zoo', 'zucchero'}
# proparoxytones (stress on the third-to-last vowel) and other non-penultimate words of our texts and commands;
# the digit is the stressed vowel counted from the end (3 = third-to-last)
STRESS = {
    'piccoli': 3, 'semplice': 3, 'subito': 3, 'povero': 3, 'dandosi': 3, 'piangere': 3, 'accendere': 3,
    'credere': 3, 'mettono': 3, 'trucioli': 3, 'figuratevi': 3, 'smarriti': 2, 'battiato': 2, 'telefono': 3,
    'musica': 3, 'macchina': 3, 'numero': 3, 'sabato': 3, 'domenica': 3, 'lucido': 3, 'rapido': 3,
    'portatile': 3, 'facile': 3, 'utile': 3, 'mobile': 3, 'giovane': 3, 'camera': 3, 'pagina': 3, 'ultimo': 3,
    'ultima': 3, 'pentola': 3, 'tavola': 3, 'gli': 1, 'mastr': 1,
}
# unstressed function words: no stress at all
CLITIC = {'il', 'lo', 'la', 'i', 'gli', 'le', 'un', 'uno', 'una', 'di', 'a', 'da', 'in', 'con', 'su', 'per', 'tra',
          'fra', 'e', 'o', 'ma', 'se', 'che', 'mi', 'ti', 'si', 'ci', 'vi', 'ne', 'del', 'dello', 'della', 'dei',
          'degli', 'delle', 'al', 'allo', 'alla', 'ai', 'agli', 'alle', 'dal', 'dalla', 'nel', 'nello', 'nella',
          'nei', 'nelle', 'sul', 'sulla', 'col', 'colla', 'cogli', 'non', 'me', 'te', 'lo', 'l', 'd', 'c', 'm', 't',
          'n', 's', 'po', 'dell', 'nell', 'all', 'dall', 'sull', 'coll', 'quell', 'mastr'}


def words(text):
    """Lowercase words; apostrophes split (l'uno -> l, uno) and the elided word keeps its letters."""
    t = text.lower().replace('’', "'")
    return [w for w in re.split(r"[^a-zàèéìíòóùú]+", t) if w]


def _letters(word):
    """(letter, accented-vowel-or-None)"""
    out = []
    for ch in word:
        if ch in ACCENT:
            base, q = ACCENT[ch]
            out.append((base, q))
        else:
            out.append((ch, None))
    return out


def word_phones(word):
    """List of [symbol, long, stressed] for one word."""
    L = _letters(word)
    s = ''.join(c for c, _ in L)
    n = len(s)
    ph = []                 # [sym, long, stressed, is_vowel_nucleus, accent_quality]
    i = 0

    def add(sym, long=False, vowel=False, q=None):
        ph.append([sym, long, False, vowel, q])

    while i < n:
        c = s[i]
        nx = s[i + 1] if i + 1 < n else ''
        nx2 = s[i + 2] if i + 2 < n else ''
        prev_v = i > 0 and s[i - 1] in VOWELSET
        if c in VOWELSET:
            q = L[i][1]
            # unstressed i/u before another vowel: glide (not when accented)
            if c in 'iu' and q is None and nx in VOWELSET and n > 1:
                if c == 'i' and i > 0 and s[i - 1] not in VOWELSET or c == 'i' and i == 0:
                    add('j'); i += 1; continue
                if c == 'u' and (i == 0 or s[i - 1] not in VOWELSET):
                    add('w'); i += 1; continue
            add(c if q is None else q, vowel=True, q=q)
            i += 1
            continue
        if c == 'h':
            i += 1
            continue
        double = nx == c
        if double:
            i += 1
            nx = s[i + 1] if i + 1 < n else ''
            nx2 = s[i + 2] if i + 2 < n else ''
        if c == 'c':
            if nx == 'h':
                add('k', double); i += 2; continue
            if nx in 'ei':
                add('tS', double)
                i += 2 if nx == 'i' and nx2 in VOWELSET and L[i + 1][1] is None else 1
                continue
            add('k', double); i += 1; continue
        if c == 'g':
            if nx == 'h':
                add('g', double); i += 2; continue
            if nx == 'l' and nx2 == 'i':
                add('L', True)
                after = s[i + 3] if i + 3 < n else ''
                i += 3 if after in VOWELSET and L[i + 2][1] is None else 2
                continue
            if nx == 'n':
                add('J', True); i += 2; continue
            if nx in 'ei':
                add('dZ', double)
                i += 2 if nx == 'i' and nx2 in VOWELSET and L[i + 1][1] is None else 1
                continue
            add('g', double); i += 1; continue
        if c == 's':
            if nx == 'c' and nx2 in 'ei':
                add('S', True)
                after = s[i + 3] if i + 3 < n else ''
                i += 3 if nx2 == 'i' and after in VOWELSET and L[i + 2][1] is None else 2
                continue
            if double:
                add('s', True); i += 1; continue
            voiced = (prev_v and nx in VOWELSET) or nx in VOICED
            add('z' if voiced else 's'); i += 1; continue
        if c == 'z':
            add('dz' if word in DZ_WORDS else 'ts', True); i += 1; continue
        if c == 'q':
            add('k', double)
            if nx == 'u':
                add('w'); i += 2
            else:
                i += 1
            continue
        if c == 'x':
            add('k'); add('s'); i += 1; continue
        if c == 'j':
            add('j'); i += 1; continue
        if c == 'y':
            add('i', vowel=True); i += 1; continue
        if c == 'k':
            add('k', double); i += 1; continue
        if c == 'w':
            add('w'); i += 1; continue
        if c in 'bdfvlmnprt':
            add(c, double); i += 1; continue
        i += 1
    # stress
    nuclei = [k for k, p in enumerate(ph) if p[3]]
    if nuclei and word not in CLITIC:
        acc = [k for k in nuclei if ph[k][4] is not None]
        if acc:
            st = acc[-1]
        elif word in STRESS:
            st = nuclei[max(0, len(nuclei) - STRESS[word])]
        else:
            st = nuclei[-2] if len(nuclei) >= 2 else nuclei[-1]
        ph[st][2] = True
        if ph[st][4] is None and ph[st][0] in 'eo':
            ph[st][0] += '*'          # stressed mid vowel, quality unknown
    # ʃ ɲ ʎ ts dz are long only between vowels
    for k, p in enumerate(ph):
        if p[0] in ('S', 'J', 'L', 'ts', 'dz') and not (k > 0 and ph[k - 1][3] and k + 1 < len(ph) and ph[k + 1][3]):
            p[1] = False if not (k > 0 and ph[k - 1][3]) else p[1]
    return [(p[0], p[1], p[2]) for p in ph]


def phones(text):
    """Phones of a sentence, words joined (Italian links words; pauses are left to the aligner)."""
    out = []
    for w in words(text):
        out += word_phones(w)
    return out


def show(ph):
    return ' '.join(p + (':' if l else '') + ("'" if s else '') for p, l, s in ph)


# hand-written expectations: word -> transcription as show() prints it
TESTS = {
    'casa': "k a' z a", 'sera': "s e*' r a", 'bene': "b e*' n e", 'città': "tS i t: a'", 'perché': "p e r k e'",
    'chiama': "k j a' m a", 'gnocchi': "J o*' k: i", 'sveglia': "z v e*' L: a", 'gli': "L i", 'scena': "S e*' n a",
    'sciare': "S a' r e", 'ciliegia': "tS i l j e*' dZ a", 'giorno': "dZ o*' r n o", 'mamma': "m a' m: a",
    'zero': "dz E' r o".replace("E'", "e*'"), 'pezzo': "p e*' ts: o", 'quando': "k w a' n d o", 'uomo': "w o*' m o",
    'piccoli': "p i' k: o l i", 'subito': "s u' b i t o", 'più': "p j u'", 'però': "p e r O'", 'naso': "n a' z o",
    'falegname': "f a l e J: a' m e", 'aglio': "a' L: o", 'figlio': "f i' L: o", 'ascia': "a' S: a",
    'pianto': "p j a' n t o", 'qui': "k w i'", 'tramontana': "t r a m o n t a' n a", 'vocina': "v o tS i' n a",
    'sbagliato': "z b a L: a' t o", 'svegliami': "z v e L: a' m i".replace("e L: a'", "e L: a'"),
}
TESTS['svegliami'] = "z v e L: a' m i"


def main():
    if sys.argv[1:] == ['--test']:
        bad = 0
        for w, want in TESTS.items():
            got = show(word_phones(w))
            if got != want:
                bad += 1
                print(f'  {w:12s} got {got:28s} want {want}')
        print(f'{len(TESTS) - bad}/{len(TESTS)} ok')
        return
    for a in sys.argv[1:]:
        print(a, '->', show(phones(a)))


if __name__ == '__main__':
    main()
