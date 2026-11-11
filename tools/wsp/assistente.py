#!/usr/bin/env python3
"""Assistente's command network, written by hand ("weight-space programming", rikiyat.github.io 2026-11-02).

Nothing here is trained. Every weight starts at 0 and is set by the construction below, which says what each unit
computes. The network reads the words of one Italian sentence (from the phone's offline recognizer or typed) and gives:
  - the intent: one of the assistant's functions, or "online" (ask Gemini);
  - a tag per word: qty (a number with a unit after it), hour, minute, span (a name/free text: contact, artist,
    city, note text, label).
The C++ side (apps/Assistente/Assistente/Core/Wsp.cpp) runs the same forward pass; Core/Command.cpp turns the tagged
words into the function's arguments (numbers, times, days are plain code).

Layers (all units are clip01(x) = min(max(x, 0), 1) unless said otherwise, so they behave like logic gates):
  0. embedding  word -> F binary features (word classes, chosen by hand)
  1. context    1-D convolution over offsets -3..+1: C units (copies of the features + "number before a unit",
                "number after alle", ...)
  2. tokens     per word, from [layer 1 ; causal mean of layer 1 over the earlier words] -> T units
                (the causal mean is one attention head with constant scores: "has an opener word come before?")
  3. rules      from [max over words of layer 1 ; max of layer 2 ; mean of layer 2] -> R units, each an AND of
                features with NOTs: clip01(sum(all) - (n - 1) - 2 * sum(none))
     Every rule also loses 1.5 x the share of unexplained words (open words with no opener before them).
  4. intents    linear: logit = 2 * (its rules); "online" is a constant 1. The phone acts only when the best
                intent beats both online and the runner-up by 0.5.

Usage: assistente.py            writes the weights (apps/Assistente/Assistente/Assets/intent.wsp)
       assistente.py --check    also runs golden.tsv and the grammar samples through the numpy forward pass
       assistente.py --ask "metti un timer di 5 minuti"
"""
import os
import re
import struct
import sys
import unicodedata

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(REPO, "apps", "Assistente", "Assistente", "Assets", "intent.wsp")
MAX_TOKENS = 96          # long spoken sentences: the request often comes after 30+ words of chatter
PRE_W = MAX_TOKENS + 4   # weight on "pre:x": the causal mean is >= 1/MAX_TOKENS when one earlier word had x
TRIGRAM_BUCKETS = 1024   # layer -1: character trigrams of a word (FNV-1a hash) -> nearest known word
FUZZY_MIN = 0.62         # cosine needed to read an unknown word as a known one ("tmer" -> "timer")
FUZZY_MIN_LEN = 4
OFFSETS = (-3, -2, -1, 0, 1)

# ---------------------------------------------------------------------------------------------------------------
# Tokenizer (Core/Wsp.cpp Tokenize does exactly the same)

UNITS = ["zero", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove"]
TEENS = ["dieci", "undici", "dodici", "tredici", "quattordici", "quindici", "sedici", "diciassette", "diciotto",
         "diciannove"]
TENS = {2: "venti", 3: "trenta", 4: "quaranta", 5: "cinquanta", 6: "sessanta", 7: "settanta", 8: "ottanta",
        9: "novanta"}


def number_word(w):
    """Italian number word 0-99 -> value, else None. "un"/"una" count as 1."""
    if w in ("un", "una", "uno"):
        return 1
    if w in UNITS:
        return UNITS.index(w)
    if w in TEENS:
        return 10 + TEENS.index(w)
    for t, name in TENS.items():
        if w == name:
            return t * 10
        for stem in (name, name[:-1]):  # "venti"+"cinque", "vent"+"uno"/"otto"
            if w.startswith(stem):
                rest = w[len(stem):]
                if rest in UNITS[1:]:
                    if (rest in ("uno", "otto")) != (stem == name[:-1]):
                        continue
                    return t * 10 + UNITS.index(rest)
    return None


def trigrams(word):
    """bucket ids of the character trigrams of ^word$ (bytes of the UTF-8 folded word), FNV-1a 32 bit"""
    b = ("^" + word + "$").encode("utf-8")
    out = set()
    for i in range(len(b) - 2):
        h = 2166136261
        for c in b[i:i + 3]:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        out.add(h % TRIGRAM_BUCKETS)
    return sorted(out)


def fold(text):
    """lowercase, accented vowels to plain ones (lunedì -> lunedi), typographic apostrophe to '."""
    out = []
    for ch in text.lower():
        if ch in "’‘":
            ch = "'"
        d = unicodedata.normalize("NFD", ch)
        if d[0] in "aeiou" and len(d) > 1:
            ch = d[0]
        out.append(ch)
    return "".join(out)


def tokenize(text):
    """-> list of (vocab word, value, original piece). value: int for <num>, (h, m) for <time>, else None."""
    s = fold(text)
    s = re.sub(r"(\d)[:.](?=\d)", lambda m: m.group(1) + "\x01", s)  # keep 7:30 / 7.30 together
    pieces = [p for p in re.split(r"[\s,.;:!?\"()'«»…]+", s) if p]
    toks = []
    for p in pieces:
        p = p.replace("\x01", ":")
        m = re.fullmatch(r"(\d{1,2}):(\d{2})", p)
        if m and int(m.group(1)) < 24 and int(m.group(2)) < 60:
            toks.append(("<time>", (int(m.group(1)), int(m.group(2))), p))
        elif re.fullmatch(r"\d{1,4}", p):
            toks.append(("<num>", int(p), p))
        elif p == "mezzogiorno":
            toks.append(("<time>", (12, 0), p))
        elif p == "mezzanotte":
            toks.append(("<time>", (0, 0), p))
        elif number_word(p) is not None:
            toks.append(("<num>", number_word(p), p))
        else:
            toks.append((p, None, p))
    # "un secondo" is a moment ("ti chiederei di un secondo dirmi l'ora"), not one second
    for k in range(len(toks) - 1):
        if toks[k][2] == "un" and toks[k + 1][0] == "secondo":
            toks[k] = ("un", None, toks[k][2])
            toks[k + 1] = ("attimo", None, toks[k + 1][2])
    return toks[:MAX_TOKENS]


# ---------------------------------------------------------------------------------------------------------------
# Layer 0: the lexicon. word -> its features. Every word listed here is "known" (closed class) unless it has the
# feature "open"; words not listed are <unk>, which is open: a name or free text.

LEX = {}


def words(features, *ws, _hand=True):
    for w in ws:
        LEX.setdefault(w, set()).update(features.split())
        if _hand:
            HAND.add(w)


HAND = set()


def forms(inf, first=True):
    """the spoken forms of a regular verb from its infinitive, as Vosk writes what people say: the imperative and
    the present ("mi mette", "chiamo"), the infinitive after "puoi/vorrei" ("svegliarmi"), the conditional and the
    subjunctive of polite requests ("mi metteresti", "che mi svegliassi"), with the pronouns attached.
    first=False leaves out the first person (for "ricordo che...": a memory, not a request)."""
    st, conj = inf[:-3], inf[-3:]
    h = "h" if conj == "are" and st.endswith(("c", "g")) else ""
    v = {"are": "a", "ere": "e", "ire": "i"}[conj]
    fut = "e" if conj in ("are", "ere") else "i"
    out = [inf, st + h + "i", st + h + "iamo", st + h + fut + "rai", st + h + fut + "resti", st + h + fut + "rebbe",
           st + h + fut + "ro", st + v + "ndo" if conj == "are" else st + "endo"]  # no -ate/-ete: "sveglie impostate"
    if first:
        out.append(st + "o")
    out.append(st + ("a" if conj == "are" else "e"))                     # mette, chiama
    imp = st + ("a" if conj == "are" else "i")                            # metti, chiama
    out.append(imp)
    out += [st + ("assi" if conj == "are" else "essi" if conj == "ere" else "issi"),
            st + ("asse" if conj == "are" else "esse" if conj == "ere" else "isse")]
    for c in ("mi", "ti", "lo", "la", "li", "le", "ci", "melo", "mela", "meli", "ne"):
        out += [inf[:-1] + c, imp + c]
    return out


GENERATED = set()  # forms made by forms(): not targets of the typo layer ("cane" is not "cancellane")


def verb(features, *infs, first=True):
    for inf in infs:
        fs = forms(inf, first)
        GENERATED.update(fs)
        words(features, *fs, _hand=False)


# numbers and times come from the tokenizer
words("num", "<num>")
words("time", "<time>")
words("open", "<unk>")
# giro 11: an unknown word with the ending of a polite frame is part of the frame, not a name or free text:
# conditionals ("sarebbe", "andrebbe", "dispiacerebbe", "chiederei"), imperfect subjunctives ("riuscissi",
# "facessi"), superlatives and manner adverbs ("gentilissimo", "cortesemente"). Core/Wsp.cpp FrameWord: same rule.
words("filler frame", "<frame>")
words("filler frame", "sarei", "avrei", "sarebbe", "avrebbe", "saresti", "avresti")
# adjectives of politeness, at the end of a request: "... molto GENTILE", "... sei GENTILISSIMO" (Command.cpp TrimTail)
# asking verbs: after them "che" asks ("vorrei SAPERE che ore sono")
words("ask_v", "sapere", "dire", "dirmi", "dimmi", "chiedere", "chiederti", "domandare", "domandarti", "capire")
# "UN SECONDO": a moment, not a duration ("ti chiederei di un secondo dirmi l'ora"): the tokenizer leaves this "un"
# a word (tokenize, Core/Wsp.cpp Tokenize)
words("filler", "un")
# "quando PUOI": not a question
words("puoi_w", "puoi")
words("polite_adj", "gentile", "gentili", "gentilissimo", "gentilissima", "carino", "carina", "bravo", "brava")


def frame_word(w):
    """an unknown word read as <frame> by its ending (Core/Wsp.cpp FrameWord does the same)"""
    if not re.fullmatch(r"[a-z]+", w):
        return False
    n = len(w)
    for end, least in (("rebbero", 8), ("rebbe", 6), ("resti", 7), ("remmo", 7), ("reste", 7), ("rei", 6),
                       ("assero", 8), ("essero", 8), ("assi", 7), ("essi", 7), ("issi", 8), ("asse", 7), ("esse", 7),
                       ("isse", 8), ("issimo", 8), ("issima", 8), ("issimi", 8), ("issime", 8), ("mente", 8),
                       ("evole", 7), ("evoli", 7)):
        if w.endswith(end) and n >= least:
            return True
    return False
# durations
words("unit unit_s", "secondi", "secondo", "sec", "s")
words("unit unit_m", "minuti", "minuto", "min", "m")
words("unit unit_h ore_w", "ore")
words("unit unit_h ora_w", "ora")
words("unit unit_h", "h")
words("half", "mezza", "mezzo", "mezz")
words("quarter", "quarto", "quarti")
words("e_w", "e")
words("meno", "meno")
words("tra", "tra", "fra")
# time prepositions: a number after them is an hour
words("tprep", "alle", "all", "le", "delle", "dalle")
# timer
words("timer_n", "timer", "rovescia", "countdown", "cronometro", "conto")
words("status", "manca", "mancano", "resta", "restano", "rimane", "rimangono", "rimasto", "punto", "rimanente")
words("avvisa", "avvisami", "avvertimi", "sapere", "avvisare")
# verbs
words("set_v", "imposta", "impostare", "punta", "crea", "avvia", "fai", "partire", "mettere", "impostami", "setta",
      "settami", "settare", "mettime")
words("set_v music_v opener", "metti", "mettimi", "mettici", "mettimelo", "mettimela", "mettimeli")
words("music_v opener", "riproduci", "suona", "ascolta", "ascoltare", "sentire", "ascoltiamo", "sentiamo", "mettiamo")
words("cancel_v", "annulla", "cancella", "elimina", "togli", "rimuovi", "annullare", "cancellare", "eliminare",
      "togliere", "fermare", "cancellami", "cancellala", "cancellalo", "toglilo", "toglila", "toglimi", "eliminala",
      "eliminalo", "annullalo", "annullala", "levami", "leva", "levalo", "levala")
# "basta" and "via" cancel, except in "e basta", "basta così", "così via" (endings: context units below)
words("basta_w", "basta")
words("via_w", "via")
words("cancel_v toggle_v opener", "disattiva")
words("cancel_v pause", "ferma", "fermati", "stop", "stoppa", "fermala", "fermalo")
# not "telefono": "il mio telefono smette di funzionare" is the phone, not a call
words("call_v opener", "call", "chiama", "telefona", "chiamami", "chiamare", "telefonare", "telefonata", "squillo", "componi",
      "parlare", "chiami", "telefonami", "telefoni", "chiamiamo", "contatta", "richiama", "contattare", "richiamare",
      "chiamata")
words("call_kind", "cellulare", "casa", "lavoro", "ufficio", "fisso")
words("open_v opener", "apri", "aprimi", "aprimelo", "aprimela", "lancia", "vedere", "aprire")
# "vai": opens only with a target ("vai su bluetooth"); "vai avanti" is the music
words("go opener", "vai")
words("toggle_v opener", "attiva", "accendi")
words("toggle_v cancel_v opener", "spegni")
words("settings_n opener", "impostazioni", "impostazione", "settings", "setting")
words("web_v opener", "cerca", "cercami", "googla")
words("remind_v opener", "ricordami", "ricordarmi", "ricorda", "promemoria")
words("note_n opener", "nota", "appunto", "note", "onenote")
# "scrivi/salva X nelle note": the content comes before the note word
words("write_v opener", "scrivi", "scrivere", "scrivimi", "salva", "salvi", "salvare", "salvami", "segnare", "annotare",
      "appuntare", "segnalo", "scrivilo", "salvalo")
words("note_v opener", "appunta", "annota", "segnati", "segna", "annotami", "appuntami", "segnami")
words("add_v", "aggiungi", "inserisci", "fissa")
words("list_v opener", "elenca", "leggi", "leggimi", "mostrami", "dimmi")
words("dimmi", "dimmi", "dici", "dica", "dirmi")
# the same verbs in every person and with pronouns: Vosk writes what was said ("tu mi mette", "puoi svegliarmi")
verb("set_v", "impostare", "settare", "avviare")
verb("set_v", "puntare", first=False)  # "a che punto è il timer"
verb("set_v music_v opener", "mettere")
verb("music_v opener", "suonare", "ascoltare")
words("music_v opener", "riproduce", "riproduco", "riprodurre", "riprodurmi", "riproducimi", "riproduciamo",
      "riprodurresti", "riprodurrai", "riproducessi", "riprodurlo", "riprodurla", "riproducilo", "riproducila")
verb("cancel_v", "annullare", "cancellare", "eliminare", "rimuovere", "levare", "sbarazzare", "staccare")
words("cancel_v", "sbarazzati", "sbarazzarti", "sbarazzarsi")
# near a verb of ours only by its letters: not typos of it ("ti COMPORTI come" is not "componi")
words("tel_w", "telefono")
words("a_prep", "a", "ad", "al", "alla", "allo")
words("filler", "comporti", "comportarti", "comportati", "comportamento", "comporta", "telefono", "telefoni",
      "telefonino", "cellulari", "smartphone")
words("cancel_v", "toglie", "tolgo", "togliere", "togliermi", "toglierlo", "toglierla", "toglieresti", "togliessi")
verb("cancel_v pause", "fermare", "stoppare")
verb("call_v opener", "chiamare", "contattare", "richiamare")
verb("call_v opener", "telefonare", first=False)
words("call_v opener", "comporre", "compone", "componimi", "comporresti")
verb("open_v opener", "aprire", "lanciare")
verb("toggle_v opener", "attivare", "accendere")
verb("web_v opener", "cercare")
verb("remind_v opener", "ricordare", first=False)
verb("write_v opener", "scrivere", "segnare", "appuntare")
verb("write_v opener", "salvare", first=False)  # "telefona a Salvo"
verb("note_v opener", "annotare")
words("add_v", "aggiunge", "aggiungere", "aggiungimi", "aggiungermi", "aggiungeresti", "aggiungilo", "inserire",
      "inserisce", "inseriscimi")
verb("resume", "riprendere", "continuare")
words("resume", "ripartire", "riparte", "ripartono")
verb("next", "saltare", "cambiare", "passare")
verb("alarm_n", "svegliare")
verb("avvisa", "avvisare", "avvertire")
# nouns
words("alarm_n", "sveglia", "svegliami", "svegliarmi", "svegli", "svegliare", "svegliate", "allarme", "allarmi")
words("alarm_n alarm_pl target", "sveglie")
# music nouns open a name: "musica rock", "canzoni di Mina"
words("music_n opener", "musica", "canzone", "canzoni", "brano", "brani", "album", "playlist", "artista", "disco")
words("music_n target opener", "musica")
words("random", "caso", "casuale", "qualcosa")
words("resume", "riprendi", "continua", "ripartire", "play", "riparti", "ricomincia", "rimetti")
words("pause", "pausa", "zitto", "zitta", "silenzio", "muto", "muta")
words("next", "avanti", "prossima", "prossimo", "successiva", "successivo", "salta", "cambia", "passa", "dopo", "skip",
      "next", "saltala")
words("prev", "indietro", "precedente", "prima")
words("filler", "torna")
words("cal_n", "calendario", "agenda", "impegni", "impegno", "appuntamenti", "appuntamento", "libero", "impegnato",
      "impegnata", "occupato", "occupata")
# "eventi", "in programma": mine only with "ho/c'è/mio" ("cosa c'è in programma domani"), else the town's events
# ("che eventi ci sono a Siracusa questo weekend": Google)
words("cal_weak", "eventi", "evento", "programma", "riunione", "riunioni", "incontro", "incontri")
words("mine", "mio", "mia", "miei", "mie")
# places around: "eventi NELLA MIA ZONA", "VICINO a me"
words("place_n", "zona", "vicinanze", "vicino", "vicini", "citta", "quartiere", "paese", "locali", "corso", "dintorni")
# changing the calendar is not reading it: "cancella gli eventi di domani", "prenota", "invita"
words("cal_change", "prenota", "prenotare", "pianifica", "pianificare", "organizza", "organizzare", "calendarizza",
      "includi", "invita", "invitare", "sposta", "spostare", "rimanda", "rinvia", "libera", "liberare", "blocca",
      "riserva", "programmare", "programmi", "inserisci", "segnare", "segna", "fissare", "svuota", "pulisci",
      "formatta", "ignora", "modifica", "crea", "creare", "nuovo", "nuova", "prendere", "aggiorna",
      "cancellato", "annullato", "rimosso", "spostato")
# making a new event (also cal_change: never a reading)
words("cal_new cal_change", "crea", "creare", "creami", "nuovo", "nuova", "prenota", "prenotare", "pianifica",
      "organizza", "calendarizza", "includi", "inserisci", "fissa", "fissami", "fissare", "programmare")  # not "in programma"
# the text of an alarm: "con il TESTO ...", "con SCRITTO ...", "che DICE ..."
words("label_w opener", "testo", "scritto", "titolo", "nome", "etichetta", "messaggio", "dice", "descrizione")
words("place_n", "centro", "biblioteca", "partite", "partita", "concerti", "concerto", "spettacoli", "mostre", "fiera")
words("cal_n target opener", "calendario")
words("opener", "agenda")  # "aggiungi al calendario venerdì CENA DA GIOVANNI": the title
words("weather_n", "bel", "brutto", "tempo", "piove", "piovera", "pioggia", "temperatura", "gradi", "caldo",
      "freddo", "nuvoloso", "ombrello", "fara", "sole", "neve", "nevica", "vento")
# "meteo Milano", "previsioni Roma": the city follows directly
words("weather_n opener", "previsioni")
words("weather_n target opener", "meteo")
words("day", "domattina", "oggi", "domani", "dopodomani", "stasera", "stanotte", "stamattina", "lunedi", "martedi", "mercoledi",
      "giovedi", "venerdi", "sabato", "domenica", "weekend", "settimana")
words("part", "mattina", "mattino", "pomeriggio", "sera", "notte")
words("repeat", "ogni", "sempre", "feriali", "giorni", "fine")
words("giorno_w", "giorno", "data")
# questions
words("q", "chi", "cosa", "perche", "come", "com", "quando", "dove", "qual", "quale", "quali", "quanti",
      "quante")
# "che" asks only when nothing was requested before it and no wish leads to it (tag che_q below)
words("che_w", "che")
words("cosa_w", "cosa")
# "ora" is the hour only after these ("che ora è", "dimmi l'ora", "un'ora"); else it is "now" ("ora mettimi...")
words("oraprev", "che", "l", "quale", "qual", "<num>", "mezz", "dimmi", "dirmi", "a")
words("q quanto", "quanto", "quanta")
words("have", "ho", "abbiamo", "devo", "c", "ha", "hai", "ci", "aspetta", "siamo")
words("ne", "ne")
# not my music: the radio, podcasts go to Gemini
words("radio", "radio", "podcast", "podcasts", "audiolibro", "audiolibri", "audible", "stazione", "stazioni", "canale",
      "canali", "spotify", "episodio", "episodi", "puntata", "puntate", "libro", "libri", "notizie", "giornale",
      "show", "trasmissione", "programmi", "youtube", "sirius", "fm", "tunein", "deezer", "pandora", "gioco", "giochi")
# about a song, not playing one: "salva questa canzone", "la mia canzone preferita", "dimmi il titolo di questo brano"
words("about_song", "preferita", "preferito", "preferite", "preferiti", "stelle", "giudizio", "voto", "like",
      "titolo", "nome", "identifica", "riconosci", "scarica", "scaricare", "testo", "playlist_add", "questa",
      "questo", "quella", "quello", "chi")
# messages: "manda un'email a papà e chiedigli com'è il tempo" is an email, not the weather
words("msg_n", "messaggio", "messaggi", "sms", "whatsapp", "manda", "mandare", "invia",
      "inviare", "inviale", "invialo", "mandagli", "chiedigli", "chiedile", "rispondi", "inoltra", "tweet", "post",
      "pubblica", "facebook", "instagram", "twitter", "contatti", "contatto", "rubrica")
# "devo mettere il cappotto / la giacca / i guanti": what to wear is the weather
words("wear_n", "cappotto", "giacca", "giubbotto", "guanti", "sciarpa", "cappello", "stivali", "sandali", "crema",
      "solare", "maglione", "piumino", "impermeabile", "pesante", "leggero", "vestirmi", "vestire", "copro", "coprirmi")
# where a new request can start in a long sentence (Vosk writes no commas): after these discourse markers
# ("... come va la vita PERÒ giusto il punto è CHE devi chiamare mia madre"). Core/Command.cpp Understand also
# starts after a "che" that does not ask, and after "a parte/detto QUESTO".
words("joint", "pero", "comunque", "quindi", "poi", "intanto", "anyway", "invece", "insomma", "vabbe", "vabbeh",
      "dunque", "inoltre", "dicevo", "cioe", "praticamente", "pratica", "ecco", "senti", "ascolta", "ascoltami",
      "beh", "allora", "ma", "stare", "perdere", "importa", "noi", "modo", "caso", "sia", "niente", "cosa", "ok",
      "okay", "bene", "comunque", "anzi", "piuttosto", "ah", "eh", "ehm", "oh", "ora", "adesso", "sentimi",
      "guarda", "aspetta", "scusa", "scusami", "dai", "infatti", "percio", "sicche", "tipo", "ciao", "mbare",
      "minchia", "raga", "ue", "ao", "belin", "figa")
# "devo mettere il cappotto": a verb after a duty is not an order to me
words("must", "devo", "dovrei", "dovremmo", "bisogna", "conviene", "posso", "potrei", "dobbiamo")
words("neg", "non")
words("si_w", "si")          # "si chiama Fido" is a name, not a call
words("habit", "sempre", "spesso", "mai", "solito", "solitamente")  # "mio papà chiama sempre alle 8"
words("cosi_w", "cosi")      # "basta così" closes the conversation
# statements, not requests: they talk ABOUT music, weather, alarms ("il meteo dice che piove sempre")
words("stmt", "dice", "dicono", "detto", "canta", "cantano", "cantava", "suonato", "suonata", "chiamato", "persa", "svegliato", "svegliata", "presto",
      "perse",
      "era", "erano", "ieri", "bellissima", "bellissimo", "bella", "piace", "piaceva", "odio", "amo", "adoro",
      "stato", "stata", "fatto", "dimenticato", "scritto", "inventato", "vinto", "morto", "nato", "troppo", "alta",
      "alto", "forte", "bassa", "basso", "brutta", "noiosa", "noioso")
# what "apri ..." can open (Actions.cpp Targets); they are open words, so they become a span
words("target open", "wi-fi", "wifi", "bluetooth", "aereo", "rete", "posizione", "gps", "batteria", "risparmio",
      "schermata", "blocco", "rotazione", "schermo", "email", "account", "orologio", "telegram", "posta",
      "registratore", "file", "perle", "tv", "authenticator", "appunti", "esplora", "modalita", "dati")
# openers that are also plain prepositions: a name or a city can follow
words("opener", "a", "ad", "per", "in", "su", "di", "x")
# spoken fillers and Sicilian/colloquial openers: they say nothing about the command
words("filler", "minchia", "mbare", "cioe", "dire", "tipo", "allora", "senti", "ehi", "praticamente", "insomma", "boh",
      "vabbe", "scusa", "beh", "ecco", "comunque", "raga", "oh", "ue", "dai", "niente", "cosi", "quindi", "pure",
      "anche", "magari", "gentilmente", "prego", "veloce", "velocemente")
# the rest of the closed class: articles, prepositions, fillers
words("filler", "il", "lo", "la", "i", "gli", "l", "del", "della", "dello", "dei", "degli", "al", "alla", "ai",
      "agli", "da", "dal", "dalla", "nel", "nella", "nei", "con", "o", "mi", "ti", "ci", "si", "me", "po", "puoi",
      "potresti", "vorrei", "voglio", "favore", "grazie", "sono", "fa", "app", "applicazione",
      "sul", "sulla", "fammi", "dammi", "d", "volta", "sai", "ok", "va", "bene", "adesso",
      "subito", "ancora", "tutte", "tutti", "mio", "mia", "miei", "mie", "questa", "questo", "prossimi", "fare",
      "prendi", "impostato", "impostata", "serve", "suona", "impostate", "impostati", "piu", "fuori", "prendere",
      "nelle", "negli", "sulle", "sugli", "google", "internet", "su", "sai", "potresti", "riesci", "numero", "messo",
      "messa", "attive", "attiva", "fatte", "sara", "sulla", "nello", "sullo", "rapida", "rapido", "tutto", "tutta",
      "piacere", "cortesia", "please", "faresti", "vorresti", "metteresti", "dovresti", "potete", "sn", "cmq", "tt",
      "pls", "plz", "impostarmi", "mettermi", "suonare", "passano", "passati", "trascorsi", "quando", "te", "tu",
      "mettila", "mettilo", "check", "guaglio", "guagliu", "anvedi", "belin", "figa", "ao", "guarda", "io", "stavo",
      "pensando", "potrei", "venuto", "mente", "trovo", "sacco", "cose", "na", "metti su")
# spoken Italian around a request (phone tests 2026-11-03 evening, Vosk text): openings, asking for the favour,
# hesitations, exclamations. They say nothing about the command, so they must not count as unexplained words.
words("filler", "eh", "ehm", "ehi", "uhm", "mh", "mm", "ah", "uh", "ahh", "ehh", "boh", "bah", "vabbe", "vabbene",
      "assistente", "ciao", "buongiorno", "buonasera", "salve", "pronto", "aspetta", "aspettami", "attimo", "momento",
      "istante", "vediamo", "vedi", "guardi", "ascoltami", "sentimi", "scusami", "scusate", "dunque", "ebbene",
      "sostanzialmente", "fondamentalmente", "diciamo", "insomma", "pratica", "volevo", "volevi", "vuoi", "voglio",
      "vorrei", "vorremmo", "chiederti", "chiedere", "chiedo", "domandarti", "domanda", "bisogno", "piacerebbe",
      "piacere", "dispiace", "dispiacerebbe", "riesci", "riusciresti", "sapresti", "puo", "posso", "potrei",
      "devi", "dovresti", "deve", "farmi", "farti", "fai", "faccia", "facciamo", "favore", "cortesia", "raccomando",
      "gentile", "gentilmente", "gentilissimo", "carino", "caro", "cara", "amico", "amica", "bello", "ringrazio",
      "mille", "tante", "tanto", "molto", "proprio", "davvero", "veramente", "solo", "soltanto", "almeno", "forse",
      "intanto", "appena", "gia", "poi", "dopo", "ecco", "certo", "esatto", "sicuro", "giusto", "allora", "bene",
      "porca", "miseria", "mannaggia", "cavolo", "accidenti", "caspita", "cacchio", "uffa", "madonna", "dio",
      "cribbio", "perbacco", "evvai", "vai", "su", "tu", "lei", "voi", "noi", "vi", "loro", "mio", "tuo",
      "mica", "pure", "neanche", "nemmeno", "ok", "okay", "okei", "va", "vada", "sia", "sei", "essere", "puoi",
      "potete", "potresti", "potreste", "vedo", "fretta", "ringraziamento", "gia", "eccomi", "farlo", "farla",
      "farmelo", "farmela", "fallo", "falla", "fammelo", "faccio", "fate", "andare", "andiamo", "vado", "andresti", "no", "nono", "sisi")
# closing words: where a name, a city or an artist ends ("chiama mamma LO PUOI FARE", "a catania PORCA MISERIA").
# Not for free text (notes, reminders, searches): there "che" and "se" belong to the text.
words("coda", "grazie", "mille", "tante", "favore", "piacere", "cortesia", "dai", "subito", "intanto", "ok", "okay",
      "porca", "mannaggia", "cavolo", "accidenti", "se", "che", "perche", "potresti", "puoi", "riesci", "ringrazio",
      "cosi", "niente", "basta", "adesso", "ora", "va", "amico", "please", "pls", "plz", "su", "quando",
      "visto", "tanto", "perfavore", "ma", "oppure", "prima", "dopo", "poi", "mentre", "almeno", "magari",
      "miseria", "ringrazio", "sapresti", "vorresti", "riusciresti", "intanto", "mannaggia")
# "vorrei CHE tu mi mettessi": after a wish "che" is a conjunction, not a question
# (not "dire che", "chiedere che": "mi sai dire che ore sono" asks)
words("wish", "vorrei", "voglio", "volevo", "vorresti", "vuoi", "piacerebbe", "bisogno", "desidero", "preferirei",
      "preferisco", "modo", "importante", "basta", "serve", "servirebbe", "ricordati", "pensando")
# "se non ti dispiace" is not a "non"
words("se_w", "se")
words("ti_w", "ti")
# chat spelling
words("che_w oraprev", "ke")
words("q", "xke", "qnd")
words("neg", "nn")

# Giro 10 (2026-11-03 notte, cortesi-a.tsv): polite indirect requests. "caro assistente mi piacerebbe se tu mi
# dicessi...", "non è che riusciresti a...", "mi faresti una ricerca su internet su...", "una nota con scritto...".
# The address: "caro MIO assistente", "amico MIO" is not "mine" (context unit below); Vosk hears "il mio agente".
words("address filler", "caro", "cara", "amico", "amica", "assistente", "gentilissimo", "gentilissima", "agente")
# the irregular verbs of a polite frame, in the conditional and the imperfect subjunctive (regular ones: forms())
words("filler", "dicessi", "dicesse", "diresti", "direbbe", "potessi", "potesse", "potrebbe", "riuscissi",
      "riuscisse", "riuscirai", "riuscirebbe", "facessi", "facesse", "faresti", "farebbe", "volessi", "volesse")
# Vosk echoes of a frame word: "piacerebbe BE", "potresti CERTI", "diresti RESTI"
words("filler", "be", "certi", "resti")
# "fammi UNA RICERCA su internet su ...": the noun opens the query like "cerca"
words("web_v opener", "ricerca", "ricerche")
# reading the calendar: "leggermi gli impegni" (not "leggero")
words("list_v opener", "leggere", "leggermi", "leggeresti", "leggessi", "leggimelo", "leggimeli")
# the text of a note after "e scrivi(gli/ci)": "metti una nota su onenote E SCRIVIGLI ..."
words("write_v opener", "scrivigli", "scrivergli", "scriverci", "scrivici", "scrivendo")
# where an artist ends: "i pink floyd IN SOTTOFONDO", "mamma GENTILMENTE"
words("coda filler", "sottofondo", "gentilmente")
# "cercare QUALCHE canzone di Vasco": not part of the name
words("random", "qualche")
# "in questo MOMENTO": "questo" is now, not "this song" (about_song)
words("momento_w", "momento")

HANDWRITTEN = HAND  # the words listed by hand somewhere, even if a form also made them
FEATURES = sorted({f for fs in LEX.values() for f in fs} | {"known", "any", "nofuzzy"})
for w, fs in LEX.items():
    fs.add("any")  # every word: "is there a word here" for the context units
    if w in GENERATED and w not in HANDWRITTEN:
        fs.add("nofuzzy")
    if "open" not in fs:
        fs.add("known")
VOCAB = sorted(LEX)
F = len(FEATURES)

# ---------------------------------------------------------------------------------------------------------------
# Layer 1: context units over offsets -3..+1. Each: list of (offset, feature, weight) and a bias.

CTX = []


def ctx(name, terms, bias):
    CTX.append((name, terms, bias))


PASS_EXTRA = {
    # "basta"/"via" cancel and pause, but not as an ending: "... e basta", "basta così", "e così via"
    "cancel_v": [(0, "basta_w", 1), (-1, "e_w", -1), (1, "cosi_w", -1), (0, "via_w", 1), (-1, "cosi_w", -1)],
    "pause": [(0, "basta_w", 1), (-1, "e_w", -1), (1, "cosi_w", -1)],
    # "ora" is the hour after che/l'/un ("che ora è", "un'ora") or alone ("ora?"), not "ora mettimi..."
    "ore_w": [(0, "ora_w", 2), (-1, "oraprev", 2), (-1, "any", -1), (1, "any", -1)],
    "neg": [(-1, "se_w", -1), (1, "ti_w", -1), (1, "e_w", -1)],  # "NON è che riusciresti": a polite frame
    "music_v": [(-1, "must", -1), (-2, "must", -1)],
    # "senti UNA COSA", "posso chiederti una cosa": not a question
    "q": [(-1, "num", -1), (-1, "set_v", -1), (1, "puoi_w", -1)],  # "mettimelo COME sveglia", "QUANDO puoi"
    # (num: below, with "sei gentilissimo")
    "mine": [(-1, "address", -1), (1, "address", -1)],
    "about_song": [(1, "momento_w", -1)],
    # "SEI gentilissimo", "SEI molto gentile": you are, not six
    "num": [(1, "cosa_w", -1), (1, "polite_adj", -1), (1, "frame", -1)],  # "caro MIO assistente", "amico MIO": an address

}
for f in FEATURES:  # every feature passes through unchanged at offset 0
    if f == "ore_w":
        ctx(f, [(0, f, 4)] + PASS_EXTRA[f], -1)
    else:
        ctx(f, [(0, f, 1)] + PASS_EXTRA.get(f, []), 0)
# "vorrei che", "ho bisogno che", "voglio proprio che": a conjunction
ctx("che_conj1", [(0, "che_w", 1), (-1, "wish", 1)], -1)
ctx("che_conj2", [(0, "che_w", 1), (-1, "e_w", 1), (-2, "neg", 1)], -2)  # "non è CHE riusciresti a..."
# "vorrei TANTO che tu", "vorrei rei tanto che": the wish two or three words before, not across an asking verb
ctx("che_conj3", [(0, "che_w", 1), (-2, "wish", 1), (-1, "ask_v", -1)], -1)
ctx("che_conj4", [(0, "che_w", 1), (-3, "wish", 1), (-1, "ask_v", -1), (-2, "ask_v", -1), (-1, "wish", -1),
                  (-2, "wish", -1)], -1)
# a request word: a "che" after it starts a reason or a relative ("svegliami alle 6 che domani lavoro")
CMD = ["set_v", "music_v", "cancel_v", "call_v", "open_v", "toggle_v", "web_v", "remind_v", "note_n", "note_v",
       "write_v", "add_v", "alarm_n", "timer_n", "music_n"]
ctx("cmd", [(0, f, 1) for f in CMD], 0)
# where the request proper starts: a command word, a question word, a topic noun. Before it is the address and the
# polite frame ("sarei contento se tu...", "ciao tesoro"): its unknown words explain nothing and blame nothing.
ANCHOR = CMD + ["weather_n", "cal_n", "cal_weak", "q", "che_w", "dimmi", "list_v", "ore_w", "giorno_w", "pause",
                "resume", "next", "prev", "target", "settings_n", "go", "toggle_v", "status", "quanto", "radio",
                "msg_n", "stmt", "have", "num", "time", "avvisa"]
ctx("anchor", [(0, f, 1) for f in ANCHOR], 0)
ctx("call_tel", [(0, "tel_w", 1), (1, "a_prep", 1)], -1)  # "telefono a Francesca": a call, not the phone
ctx("qty_n", [(0, "num", 1), (1, "unit", 1)], -1)  # "5 minuti"
ctx("qty_half", [(0, "half", 1), (1, "unit", 1)], -1)  # "mezz'ora"
ctx("qty_quarter", [(0, "unit", 1), (-2, "quarter", 1)], -1)  # "un quarto d'ORA" (tagged on "ora")
ctx("hour_prep", [(0, "num", 1), (-1, "tprep", 1)], -1)  # "alle 7"
ctx("hour_ore", [(0, "num", 1), (-1, "ore_w", 1), (-2, "tprep", 1)], -2)  # "alle ore 7"
ctx("hour_per", [(0, "num", 1), (-1, "opener", 1), (-2, "alarm_n", 1)], -2)  # "sveglia per 7" (rare, typed)
ctx("hour_time", [(0, "time", 1)], 0)  # "7:30", mezzogiorno
ctx("si_call", [(0, "call_v", 1), (-1, "si_w", 1)], -1)  # "si chiama"
ctx("hour_alarm", [(0, "num", 1), (-1, "alarm_n", 1), (1, "unit", -1)], -1)  # "sveglia 6" (not "sveglia 6 minuti")
ctx("hour_alarm_day", [(0, "num", 1), (-1, "day", 1), (-2, "alarm_n", 1), (1, "unit", -1)], -2)  # "sveglia domani 6"
ctx("minute", [(0, "num", 1), (-1, "e_w", 1), (-2, "num", 1), (-3, "tprep", 1)], -3)  # "alle 7 e 20"
ctx("minute_ore", [(0, "num", 1), (-1, "e_w", 1), (-2, "num", 1), (-3, "ore_w", 1)], -3)  # "alle ore 7 e 20"
CTXN = [c[0] for c in CTX]
C = len(CTX)

# ---------------------------------------------------------------------------------------------------------------
# Layer 2: per-word tags. Input [layer 1 at this word ; causal mean of layer 1 over the earlier words].
# "pre:x" = mean of x over the words before (> 0 iff some earlier word had x). Mean >= 1/MAX_TOKENS, so weight
# PRE_W makes any earlier opener count fully.

TOK = []


def tok(name, terms, bias):
    TOK.append((name, terms, bias))


tok("qty", [("qty_n", 1), ("qty_half", 1), ("qty_quarter", 1)], 0)
tok("hour", [("hour_prep", 1), ("hour_ore", 1), ("hour_per", 1), ("hour_time", 1), ("hour_alarm", 1),
             ("hour_alarm_day", 1)], 0)
tok("minute", [("minute", 1), ("minute_ore", 1)], 0)
# span: an open word (a name, free text) after an opener: "chiama MARIO", "a PALERMO", "di COMPRARE il LATTE".
# pre <= 1, so -40 x known switches it off for every known word whatever came before.
tok("span", [("pre:opener", PRE_W), ("known", -PRE_W)], 0)
# unexplained: an open word with no opener before it, once the request has started ("chi ha SCRITTO i PROMESSI
# SPOSI"; not "ciao TESORO metti...": before the anchor it is the address)
# open AND pre:anchor AND NOT pre:opener AND NOT target. The causal means are fractions (1/n .. 1): open x PRE_W
# against -PRE_W leaves pre:anchor x PRE_W (>= 1 once any anchor came, <= PRE_W); a NOT weighs PRE_W^2, more than
# any anchor share
tok("unexplained", [("open", PRE_W), ("target", -PRE_W * PRE_W), ("pre:opener", -PRE_W * PRE_W),
                    ("pre:anchor", PRE_W)], -PRE_W)
# the text of a note: any word after a note/write word that is not an ending ("annota che giovedì c'è LA RIUNIONE":
# known words too, so a span is not needed)
tok("note_text", [("pre:note_v", PRE_W), ("pre:note_n", PRE_W), ("pre:write_v", PRE_W), ("pre:add_v", PRE_W), ("any", 1),
                  ("coda", -PRE_W), ("note_n", -PRE_W), ("note_v", -PRE_W)], -1)
# "che" as a question word: not after a wish, not after a request
tok("che_q", [("che_w", 1), ("che_conj1", -1), ("che_conj2", -1), ("che_conj3", -1), ("che_conj4", -1),
              ("pre:cmd", -PRE_W)], 0)
TOKN = [t[0] for t in TOK]
T = len(TOK)

# ---------------------------------------------------------------------------------------------------------------
# Layer 3: rules. Pooled input: "x" = max over words of layer 1 (did any word have x), "tag:x" = max of the tag,
# "mean:x" = mean of the tag. Each rule = AND(all) AND NOT(any of none).

RULES = []


def rule(intent, all_, none=()):
    """a question word is "q" (chi, come, cosa...) or a "che" that asks (tag che_q): a rule that needs one is
    written twice, a rule that forbids one forbids both"""
    none = list(none) + (["tag:che_q"] if "q" in none else [])
    RULES.append((intent, list(all_), none))
    if "q" in all_:
        RULES.append((intent, [x if x != "q" else "tag:che_q" for x in all_], none))


NOT_OTHER = ["open_v", "q"]
# timer
rule("imposta_timer", ["timer_n", "tag:qty"], ["cancel_v", "status", "alarm_n", "remind_v"])
rule("imposta_timer", ["set_v", "tag:qty"], ["timer_n", "alarm_n", "remind_v", "note_n", "tag:hour", "cancel_v"])
rule("imposta_timer", ["avvisa", "tra", "tag:qty"], ["tag:span", "alarm_n", "remind_v"])
rule("imposta_timer", ["alarm_n", "tra", "tag:qty"], ["tag:hour", "cancel_v", "remind_v"])  # "svegliami tra un'ora"
rule("imposta_timer", ["avvisa", "tag:qty"], ["tag:hour", "remind_v", "alarm_n", "tra", "tag:span", "stmt"])  # "fammi sapere quando passano 25 minuti"
rule("stato_timer", ["timer_n", "status"], ["cancel_v", "set_v", "tag:qty"])
rule("stato_timer", ["timer_n", "quanto"], ["cancel_v", "set_v", "tag:qty"])
# "quanto manca?" alone: the timer is the only thing that runs out
rule("stato_timer", ["quanto", "status"], ["timer_n", "cancel_v", "set_v", "tag:qty", "alarm_n", "cal_n", "weather_n",
                                            "music_n", "tag:span", "day"])
rule("annulla_timer", ["timer_n", "cancel_v"], ["tag:qty", "status"])
rule("annulla_timer", ["timer_n", "neg"], ["tag:qty", "status", "cancel_v", "set_v"])  # "il timer non serve più"
# alarms
rule("imposta_sveglia", ["alarm_n", "tag:hour"], ["cancel_v", "q", "list_v", "remind_v", "open_v", "timer_n", "neg"])
rule("elenca_sveglie", ["alarm_n", "q"], ["tag:hour", "cancel_v", "set_v", "open_v", "timer_n", "stmt"])
rule("elenca_sveglie", ["alarm_pl"], ["tag:hour", "cancel_v", "set_v", "open_v", "timer_n", "go", "list_v", "q",
                                    "have", "toggle_v", "neg", "settings_n", "stmt"])  # "sveglie?"
rule("elenca_sveglie", ["alarm_pl", "have"], ["tag:hour", "cancel_v", "set_v", "open_v", "timer_n", "stmt"])
rule("elenca_sveglie", ["alarm_n", "list_v"], ["tag:hour", "cancel_v", "set_v", "open_v", "timer_n", "stmt"])
rule("elenca_sveglie", ["alarm_n", "have"], ["tag:hour", "cancel_v", "set_v", "open_v", "timer_n", "q", "list_v",
                                             "alarm_pl", "neg", "stmt"])  # "ho messo la sveglia per domani?"
rule("cancella_sveglia", ["alarm_n", "cancel_v", "tag:hour"], ["set_v", "timer_n"])
rule("cancella_sveglia", ["alarm_n", "neg", "tag:hour"], ["set_v", "timer_n", "cancel_v"])  # "non voglio più la sveglia delle 8"
# reminders and notes (the text is a span: only typed text has one, the offline recognizer has no dictation)
rule("promemoria", ["remind_v", "tag:hour", "tag:span"], ["cancel_v", "q"])
rule("promemoria", ["remind_v", "tra", "tag:qty", "tag:span"], ["cancel_v", "q", "tag:hour"])
# no "q" here: notes are full of "che" ("annota che il codice è 1234")
rule("prendi_nota", ["note_n", "tag:note_text"], ["remind_v", "cal_n", "open_v"])
rule("prendi_nota", ["note_v", "tag:note_text"], ["remind_v", "cal_n", "open_v", "note_n"])
rule("prendi_nota", ["write_v", "note_n", "tag:note_text"], ["remind_v", "cal_n", "open_v", "note_v"])
rule("prendi_nota", ["add_v", "note_n", "tag:note_text"], ["remind_v", "cal_n", "open_v", "note_v", "write_v"])
# music
MUSIC_NOT = ["label_w", "timer_n", "alarm_n", "remind_v", "note_n", "note_v", "cal_n", "tag:qty", "tag:hour", "open_v", "q",
             "call_v", "weather_n", "web_v", "pause", "radio", "go", "next", "prev", "resume", "stmt", "write_v",
             "note_v", "add_v", "about_song", "msg_n", "cal_weak"]
rule("riproduci_musica", ["music_v", "tag:span"], MUSIC_NOT)
rule("riproduci_musica", ["music_v", "music_n"], MUSIC_NOT)
# "cercare qualche canzone di Vasco": looking for songs is playing them (not a web search)
rule("riproduci_musica", ["web_v", "music_n", "tag:span"], [x for x in MUSIC_NOT if x != "web_v"])
rule("riproduci_musica", ["random", "music_n"], MUSIC_NOT + ["music_v"])
rule("riproduci_musica", ["music_v", "random"], MUSIC_NOT + ["music_n", "tag:span"])  # "metti qualcosa"
# "fai partire la musica", "musica!": the music noun alone
rule("riproduci_musica", ["music_n"], MUSIC_NOT + ["music_v", "random", "cancel_v", "toggle_v", "settings_n", "next",
                                                   "prev", "resume", "neg"])
CTL_NOT = ["timer_n", "alarm_n", "q", "open_v", "call_v", "remind_v", "note_n", "tag:span", "weather_n", "cal_n",
           "day", "web_v", "stmt", "radio", "msg_n", "cal_weak", "cal_change"]
rule("controlla_musica", ["pause"], CTL_NOT + ["resume"])  # "basta così" is not a pause: context unit
rule("controlla_musica", ["resume"], CTL_NOT + ["pause"])
rule("controlla_musica", ["next"], CTL_NOT + ["pause", "resume", "prev"])
rule("controlla_musica", ["prev"], CTL_NOT + ["pause", "resume", "next"])
# calls, apps, web
rule("chiama", ["call_tel"], ["q", "note_n", "note_v", "remind_v", "open_v", "web_v", "stmt", "si_call", "habit",
                              "msg_n", "cal_n", "cal_weak", "add_v", "write_v", "call_v"])
rule("chiama", ["call_v"], ["label_w", "q", "note_n", "note_v", "remind_v", "open_v", "web_v", "stmt", "si_call", "habit",
                            "msg_n", "cal_n", "cal_weak", "add_v", "write_v"])
rule("apri", ["open_v"], ["call_v", "web_v", "q", "msg_n", "cal_change"])
rule("apri", ["settings_n"], ["call_v", "web_v", "open_v", "stmt"])  # "dove sono le impostazioni wifi"
rule("apri", ["toggle_v", "target"], ["call_v", "web_v", "q", "open_v", "music_n", "settings_n", "alarm_n", "timer_n"])
rule("apri", ["go", "target"], ["call_v", "web_v", "q", "open_v", "settings_n", "toggle_v", "msg_n"])
ALL_VERBS = ["set_v", "music_v", "cancel_v", "call_v", "open_v", "go", "toggle_v", "web_v", "remind_v", "note_n",
             "note_v", "write_v", "add_v", "list_v", "settings_n"]
NOUNS = ["alarm_n", "timer_n", "weather_n", "cal_n", "music_n"]
rule("apri", ["target"], ALL_VERBS + NOUNS + ["q", "day", "tag:qty", "tag:hour", "neg", "msg_n", "cal_change"])  # "wifi"
rule("apri", ["list_v", "target"], ["call_v", "web_v", "q", "open_v", "settings_n", "toggle_v", "go", "alarm_n", "msg_n",
                                    "cal_n", "weather_n", "timer_n"])  # "mostrami il registratore"
rule("cerca_sul_web", ["web_v", "tag:span"], ["call_v", "music_n", "open_v"])
# weather, calendar
rule("meteo", ["weather_n"], ["timer_n", "alarm_n", "open_v", "cal_n", "quanto", "remind_v", "note_n", "call_v",
                              "web_v", "add_v", "set_v", "settings_n", "go", "stmt", "msg_n", "must",
                              "write_v"])
rule("meteo", ["weather_n", "must"], ["timer_n", "alarm_n", "open_v", "cal_n", "quanto", "remind_v", "note_n",
                                      "call_v", "web_v", "add_v", "set_v", "settings_n", "go", "stmt", "msg_n",
                                      "write_v"])  # "devo prendere l'ombrello"
rule("meteo", ["wear_n", "must"], ["timer_n", "alarm_n", "open_v", "cal_n", "remind_v", "note_n", "call_v", "web_v",
                                   "msg_n", "write_v"])
CAL_NOT = ["add_v", "set_v", "open_v", "settings_n", "ne", "go", "stmt", "alarm_n", "timer_n", "weather_n", "remind_v",
           "note_n", "note_v", "call_v", "cancel_v", "write_v", "cal_change", "msg_n", "radio", "place_n"]
rule("leggi_calendario", ["cal_n"], CAL_NOT)
rule("leggi_calendario", ["cal_weak", "have"], CAL_NOT + ["tag:span"])  # "eventi a Trento": the town's
rule("leggi_calendario", ["cal_weak", "mine"], CAL_NOT + ["tag:span"])
rule("leggi_calendario", ["q", "have", "day"], CAL_NOT + ["tag:span", "unit"])
# a new event in the calendar (the title is free text before or after: Core/Command.cpp FreeText)
EVT_NOT = ["q", "cancel_v", "msg_n", "radio", "alarm_n", "timer_n", "remind_v", "neg", "stmt", "call_v", "list_v",
           "weather_n", "music_n"]
for v in ("set_v", "add_v", "write_v", "cal_new"):
    rule("aggiungi_evento", ["cal_n", v], EVT_NOT)            # "metti sul calendario", "segna in agenda"
for v in ("set_v", "add_v", "cal_new"):
    rule("aggiungi_evento", ["cal_weak", v], EVT_NOT)          # "crea un evento", "aggiungi una riunione"
# "metti un evento nel calendario ... CHE SI CHIAMA scadenza tasse": that "chiama" names the title, it calls nobody
EVT_NOT_SI = [x for x in EVT_NOT if x != "call_v"]
for v in ("set_v", "add_v", "write_v", "cal_new"):
    rule("aggiungi_evento", ["cal_n", v, "si_call"], EVT_NOT_SI)
for v in ("set_v", "add_v", "cal_new"):
    rule("aggiungi_evento", ["cal_weak", v, "si_call"], EVT_NOT_SI)
# time and date, answered on the phone
TIME_NOT = ["num", "time", "timer_n", "alarm_n", "remind_v", "cal_n", "weather_n", "tag:span", "tprep", "unit_m",
            "unit_s", "stmt", "cal_weak", "mine"]  # "che giorno è la riunione", "che data è il mio anniversario"
rule("ora", ["q", "ore_w"], TIME_NOT)
rule("ora", ["dimmi", "ore_w"], TIME_NOT)
rule("ora", ["ore_w"], TIME_NOT + ALL_VERBS + NOUNS + ["q", "dimmi", "day", "part", "have", "neg", "status", "tra",
                              "avvisa", "tag:qty", "half", "quarter"])  # "ore?"
rule("data", ["q", "giorno_w"], TIME_NOT + ["ore_w", "repeat"])
rule("data", ["dimmi", "giorno_w"], TIME_NOT + ["ore_w", "repeat"])  # "dimmi la data di oggi"
rule("data", ["ne", "q", "have"], TIME_NOT + ["ore_w"])  # "quanti ne abbiamo oggi"
rule("data", ["giorno_w", "day"], TIME_NOT + ["ore_w", "repeat", "q", "have"] + ALL_VERBS)  # "data di oggi"

INTENTS = ["online"] + sorted({r[0] for r in RULES})
R = len(RULES)
I = len(INTENTS)
POOLN = CTXN + ["tag:" + t for t in TOKN] + ["mean:" + t for t in TOKN]
G = len(POOLN)


# ---------------------------------------------------------------------------------------------------------------
# Writing the weights

def fuzzy_rows():
    """the trigram layer: one column per known word, 1/sqrt(n) on its trigrams (so a dot product is a cosine).
    Only real words of 4+ letters take part (not <num>, <unk>, short function words)."""
    V = len(VOCAB)
    M = np.zeros((TRIGRAM_BUCKETS, V), np.float32)
    for v, w in enumerate(VOCAB):
        if w.startswith("<") or len(w) < FUZZY_MIN_LEN or "nofuzzy" in LEX[w]:
            continue
        g = trigrams(w)
        for h in g:
            M[h, v] = 1 / np.sqrt(len(g))
    return M


def fuzzy(M, word):
    """unknown word -> (vocab word, cosine) of the nearest known word, or (None, best)"""
    if len(word) < FUZZY_MIN_LEN or not re.fullmatch(r"[a-z'-]+", word):
        return None, 0.0
    g = trigrams(word)
    x = np.zeros(TRIGRAM_BUCKETS, np.float32)
    x[g] = 1 / np.sqrt(len(g))
    scores = x @ M
    # typos keep the first letter: "pasta" is not "basta"
    scores = scores * np.array([1.0 if w[:1] == word[:1] else 0.0 for w in VOCAB], np.float32)
    # and about the length: "contento" is not "conto" (Core/Wsp.cpp Fuzzy, giro 11)
    scores = scores * np.array([1.0 if abs(len(w) - len(word)) <= max(1, len(word) // 4) else 0.0 for w in VOCAB],
                               np.float32)
    v = int(np.argmax(scores))
    return (VOCAB[v], float(scores[v])) if scores[v] >= FUZZY_MIN else (None, float(scores[v]))


def build():
    V = len(VOCAB)
    E = np.zeros((V, F), np.float32)
    for i, w in enumerate(VOCAB):
        for f in LEX[w]:
            E[i, FEATURES.index(f)] = 1
    K = np.zeros((len(OFFSETS), F, C), np.float32)
    bK = np.zeros(C, np.float32)
    for c, (name, terms, bias) in enumerate(CTX):
        for off, f, wgt in terms:
            K[OFFSETS.index(off), FEATURES.index(f), c] = wgt
        bK[c] = bias
    A = np.zeros((2 * C, T), np.float32)
    bA = np.zeros(T, np.float32)
    for t, (name, terms, bias) in enumerate(TOK):
        for src, wgt in terms:
            if src.startswith("pre:"):
                A[C + CTXN.index(src[4:]), t] = wgt
            else:
                A[CTXN.index(src), t] = wgt
        bA[t] = bias
    B = np.zeros((G, R), np.float32)
    bB = np.zeros(R, np.float32)
    for r, (intent, all_, none) in enumerate(RULES):
        for f in all_:
            B[POOLN.index(f), r] += 1
        for f in none:
            B[POOLN.index(f), r] -= 2
        bB[r] = -(len(all_) - 1)
    O = np.zeros((R, I), np.float32)
    bO = np.zeros(I, np.float32)
    for r, (intent, _, _) in enumerate(RULES):
        O[r, INTENTS.index(intent)] = 2
    # a sentence with words nothing explains is not a command: every rule loses 1.5 x their share
    for r in range(R):
        B[POOLN.index("mean:unexplained"), r] -= 1.5
    bO[INTENTS.index("online")] = 1
    return dict(E=E, K=K, bK=bK, A=A, bA=bA, B=B, bB=bB, O=O, bO=bO, M=fuzzy_rows())


def clip01(x):
    return np.minimum(np.maximum(x, 0), 1)


def forward(net, text):
    toks = tokenize(text)
    n = len(toks)
    if n == 0:
        return toks, None, None
    ids = []
    for w, _, _ in toks:
        if w not in LEX and frame_word(w):
            w = "<frame>"
        if w not in LEX:
            near, _ = fuzzy(net["M"], w)
            w = near if near else "<unk>"
        ids.append(VOCAB.index(w))
    e = net["E"][ids]  # n x F
    h1 = np.tile(net["bK"], (n, 1))
    for k, off in enumerate(OFFSETS):
        for t in range(n):
            s = t + off
            if 0 <= s < n:
                h1[t] += e[s] @ net["K"][k]
    h1 = clip01(h1)
    pre = np.zeros_like(h1)
    for t in range(1, n):
        pre[t] = h1[:t].mean(axis=0)
    h2 = clip01(np.concatenate([h1, pre], axis=1) @ net["A"] + net["bA"])
    g = np.concatenate([h1.max(axis=0), h2.max(axis=0), h2.mean(axis=0)])
    r = clip01(g @ net["B"] + net["bB"])
    logits = r @ net["O"] + net["bO"]
    return toks, h2, logits


def decide(logits):
    """Same as Core/Wsp.cpp: the best intent if it beats online and the runner-up by 0.5."""
    order = np.argsort(-logits)
    best, second = order[0], order[1]
    if INTENTS[best] == "online" or logits[best] - logits[second] < 0.5:
        return "online", float(logits[best] - logits[second])
    return INTENTS[best], float(logits[best] - logits[second])


def write(net, path):
    header = ["WSP1",
              "dims %d %d %d %d %d %d %d %d" % (len(VOCAB), F, C, T, R, I, OFFSETS[0], OFFSETS[-1]),
              "max_tokens %d" % MAX_TOKENS,
              "fuzzy %d %.4f %d" % (TRIGRAM_BUCKETS, FUZZY_MIN, FUZZY_MIN_LEN)]
    header += ["v " + w for w in VOCAB]
    header += ["i " + x for x in INTENTS]
    header += ["t " + x for x in TOKN]
    header += ["f " + x for x in FEATURES]
    header += ["---"]
    blob = ("\n".join(header) + "\n").encode("utf-8")
    # M (the trigram layer) is not stored: Core/Wsp.cpp writes it from the vocabulary at load, by the same rule
    for key in ("E", "K", "bK", "A", "bA", "B", "bB", "O", "bO"):
        a = np.ascontiguousarray(net[key], dtype="<f4")
        blob += a.tobytes()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


def golden():
    rows = []
    with open(os.path.join(HERE, "golden.tsv"), encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            rows.append((parts[0], parts[1]))
    return rows


def check(net):
    bad = 0
    rows = golden()
    for text, want in rows:
        toks, h2, logits = forward(net, text)
        got, margin = decide(logits)
        if got != want:
            bad += 1
            print("golden FAIL: %r -> %s (%.2f), want %s" % (text, got, margin, want))
    print("golden: %d/%d" % (len(rows) - bad, len(rows)))
    sys.path.insert(0, HERE)
    import grammar
    samples = grammar.samples(4000)
    gbad = 0
    for text, want in samples:
        got, margin = decide(forward(net, text)[2])
        if got != want:
            gbad += 1
            if gbad <= 30:
                print("grammar FAIL: %r -> %s (%.2f), want %s" % (text, got, margin, want))
    print("grammar samples: %d/%d" % (len(samples) - gbad, len(samples)))
    unknown = sorted({w for text, _ in samples for w, _, _ in tokenize(text) if w not in LEX})
    print("grammar words not in the lexicon (become <unk>): %s" % " ".join(unknown[:80]))
    return bad + gbad


def main():
    net = build()
    n = write(net, OUT)
    sizes = {k: v.size for k, v in net.items()}
    print("intent.wsp: %d bytes, V=%d F=%d C=%d T=%d R=%d I=%d, %d weights" % (
        n, len(VOCAB), F, C, T, R, I, sum(sizes.values())))
    if "--ask" in sys.argv:
        text = sys.argv[sys.argv.index("--ask") + 1]
        toks, h2, logits = forward(net, text)
        for (w, v, p), tags in zip(toks, h2):
            on = [TOKN[i] for i in range(T) if tags[i] > 0.5]
            near = "" if w in LEX else "~%s %.2f" % fuzzy(net["M"], w)
            print("  %-14s %-10s %s %s" % (p, w, " ".join(on), near))
        for i in np.argsort(-logits)[:3]:
            print("  %-18s %.2f" % (INTENTS[i], logits[i]))
        print("->", decide(logits))
    if "--check" in sys.argv:
        sys.exit(1 if check(net) else 0)


if __name__ == "__main__":
    main()
