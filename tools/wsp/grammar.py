#!/usr/bin/env python3
"""The offline recognizer's grammar (SRGS, it-IT) for Assistente, and a sampler of its sentences.

The phone's SpeechRecognizer has no offline dictation: offline it only hears sentences of a grammar. This file
describes the commands once; it writes apps/Assistente/Assistente/Assets/comandi.grxml and gives random sentences of
the same grammar to tools/wsp/assistente.py --check, so everything the recognizer can return is checked against the
hand-written network.

Dynamic parts are filled on the phone (OfflineCommand.cpp) where the template says <!--DINAMICO-->:
rule "contatto" (contact names) and rule "brano" (artists and albums from the music library).

Notes, reminders, web searches and new calendar events need free text, so they are not in the grammar: the
recognizer rejects them and the audio goes to Gemini.
"""
import os
import random
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(REPO, "apps", "Assistente", "Assistente", "Assets", "comandi.grxml")


class Seq:
    def __init__(self, *items): self.items = items


class Alt:
    def __init__(self, *items): self.items = items


class Opt:
    def __init__(self, *items): self.item = Seq(*items)


class Ref:
    def __init__(self, name): self.name = name


def alt(text):  # "a|b|c" -> Alt of words
    return Alt(*text.split("|"))


UNITS = ["", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove"]
TEENS = ["dieci", "undici", "dodici", "tredici", "quattordici", "quindici", "sedici", "diciassette", "diciotto",
         "diciannove"]
TENS = ["", "", "venti", "trenta", "quaranta", "cinquanta"]


def number_text(n):
    if n < 10:
        return UNITS[n]
    if n < 20:
        return TEENS[n - 10]
    t, u = divmod(n, 10)
    stem = TENS[t]
    if u in (1, 8):
        stem = stem[:-1]
    return stem + (u"tré" if u == 3 else UNITS[u])


DAYS = [u"lunedì", u"martedì", u"mercoledì", u"giovedì", u"venerdì", "sabato", "domenica"]
CITIES = ["Catania", "Palermo", "Roma", "Milano", "Napoli", "Torino", "Firenze", "Bologna", "Venezia", "Genova",
          "Bari", "Messina", "Siracusa", "Ragusa", "Agrigento", "Trapani", "Enna", "Caltanissetta", "Taormina",
          "Acireale", "Cagliari", "Verona", "Padova", "Trieste", "Pisa", "Lecce", "Reggio Calabria", "Cosenza",
          "Londra", "Parigi", "Berlino", "Madrid", "Barcellona", "New York"]
TARGETS = ["wi-fi", "wifi", "bluetooth", u"modalità aereo", "rete cellulare", "dati", "posizione", "gps",
           "risparmio batteria", "batteria", "schermata di blocco", "rotazione schermo", "account email",
           "musica", "orologio", "sveglie", "calendario", "meteo", "telegram", "posta", "registratore", "file",
           "perle tv", "authenticator", "appunti", "esplora"]

RULES = {
    "n1_59": Alt(*[number_text(n) for n in range(1, 60)] + ["un"]),
    "n1_99": Alt(*[number_text(n) for n in range(1, 60)] + ["un"]),
    "ora_n": Alt(*[number_text(n) for n in range(1, 24)] + ["mezzogiorno", "mezzanotte"]),
    "unita": alt("secondi|secondo|minuti|minuto|ore|ora"),
    "durata": Alt(
        Seq(Ref("n1_59"), Ref("unita"), Opt("e", Alt(Seq(Ref("n1_59"), alt("minuti|secondi")), "mezza", "mezzo"))),
        "mezz'ora", "un quarto d'ora", "un'ora e mezza"),
    "minuti": Alt(Seq("e", Ref("n1_59")), "e mezza", "e un quarto", "meno un quarto", "e tre quarti",
                  Seq("meno", Ref("n1_59"))),
    "parte": alt("di mattina|del mattino|del pomeriggio|di sera|di notte"),
    "orario": Alt(Seq(alt("alle|per le|le|alle ore"), Ref("ora_n"), Opt(Ref("minuti")), Opt(Ref("parte"))),
                  Seq("all'una", Opt(Ref("minuti")), Opt(Ref("parte"))),
                  Seq(alt("a|per"), alt("mezzogiorno|mezzanotte"))),
    "giorno": Alt("oggi", "domani", "dopodomani", "stasera", "stamattina", "stanotte", *DAYS),
    "giorno2": Alt(Ref("giorno"), Seq(alt("questa|la prossima"), "settimana"), "nel weekend", "nel fine settimana"),
    "ripeti": alt("ogni giorno|tutti i giorni|nei giorni feriali|nel weekend|nel fine settimana"),
    "citta": Alt(*CITIES),
    "destinazione": Alt(*TARGETS),
    # dynamic, filled on the phone
    "contatto": Alt("mamma", "papà", "Francesca", "Mario Rossi", "dottor Bianchi", "Giulia"),
    "brano": Alt("Battiato", "Pink Floyd", "Lucio Dalla", "La voce del padrone", "Vasco Rossi", "Mina"),

    # ---- the commands (each top rule's name is the intent it must give) ----
    "imposta_timer": Alt(
        Seq(Opt(alt("metti|imposta|avvia|fai partire|puoi mettere|mettimi")), Opt(alt("un|il")), "timer",
            Opt(alt("di|da|a|per")), Ref("durata"), Opt("per", alt("la pasta|il forno|il bucato|le uova"))),
        Seq(Opt(alt("metti|imposta|avvia")), Opt("un"), "conto alla rovescia di", Ref("durata")),
        Seq(alt("avvisami|avvertimi"), "tra", Ref("durata"))),
    "stato_timer": Alt("quanto manca al timer", "quanto tempo manca al timer", "quanto resta al timer",
                       u"a che punto è il timer", "quanto manca alla fine del timer", "timer quanto manca"),
    "annulla_timer": Seq(alt("annulla|cancella|ferma|togli|elimina|stop"), Opt(alt("il|al")), "timer"),
    "imposta_sveglia": Alt(
        Seq(alt("svegliami|metti la sveglia|metti una sveglia|imposta la sveglia|imposta una sveglia|"
                "punta la sveglia|sveglia|mettimi la sveglia|puoi svegliarmi"),
            Opt(Ref("giorno")), Ref("orario"), Opt(Ref("giorno")), Opt(Ref("ripeti"))),
        Seq(alt("metti la sveglia|imposta una sveglia"), Ref("ripeti"), Ref("orario"))),
    "elenca_sveglie": Alt("quali sveglie ho", "che sveglie ho", "che sveglie ho impostato", "elenca le sveglie",
                          u"a che ora è la sveglia", "a che ora suona la sveglia", "dimmi le sveglie",
                          "quali sono le mie sveglie", "leggi le sveglie", "ho sveglie domani"),
    "cancella_sveglia": Seq(alt("cancella|elimina|togli|annulla|disattiva"), alt("la sveglia|le sveglie"),
                            alt("delle|di|alle"), Ref("ora_n"), Opt(Ref("minuti"))),
    "chiama": Seq(alt("chiama|telefona a|chiamami|puoi chiamare"), Ref("contatto"),
                  Opt(alt("al cellulare|a casa|al lavoro|al fisso|in ufficio"))),
    "riproduci_musica": Alt(
        Seq(alt("metti|riproduci|suona|fammi sentire|fammi ascoltare|metti un po' di|metti qualcosa di|"
                "voglio ascoltare|metti l'album|riproduci l'album|metti la playlist|metti le canzoni di"),
            Ref("brano")),
        Seq(alt("metti|riproduci|fammi sentire"), alt("un po' di musica|della musica|la musica|musica"),
            Opt("a caso")),
        "musica a caso", "riproduci musica casuale", "metti delle canzoni a caso"),
    "controlla_musica": Alt("pausa", "metti in pausa", "ferma la musica", "stop", "riprendi", "riprendi la musica",
                            "continua", "fai ripartire la musica", "avanti", "prossima canzone", "canzone successiva",
                            "salta", "salta canzone", "cambia canzone", "indietro", "canzone precedente",
                            "torna indietro"),
    "apri": Alt(Seq(alt("apri|aprimi|lancia|vai su|vai in"),
                    Opt(alt("l'app|le impostazioni|le impostazioni del|le impostazioni della|il|la|lo|le|gli")),
                    Ref("destinazione")),
                Seq("impostazioni", Ref("destinazione")),
                Seq(alt("attiva|disattiva|accendi|spegni"), Opt(alt("il|la|lo|i")),
                    alt("wi-fi|wifi|bluetooth|modalità aereo|gps|posizione|risparmio batteria|dati"))),
    "meteo": Alt(
        Seq(alt("che tempo fa|che tempo farà|com'è il tempo|com'è il meteo|meteo|il meteo|"
                "previsioni del tempo|piove|pioverà|che temperatura c'è|quanti gradi ci sono|"
                "serve l'ombrello"),
            Opt(Ref("giorno2")), Opt(alt("a|per|su|in"), Ref("citta")), Opt(Ref("giorno2"))),
        Seq("meteo", Ref("citta"), Opt(Ref("giorno2")))),
    "leggi_calendario": Alt(
        Seq(alt("cosa ho|che cosa ho|cosa devo fare|che impegni ho|ho impegni|ho appuntamenti|"
                "che appuntamenti ho|cosa c'è in agenda|cosa c'è nel calendario"),
            Ref("giorno2")),
        Seq(alt("leggi il calendario|leggimi gli impegni|dimmi gli impegni|impegni|appuntamenti|agenda"),
            Opt(alt("di|per|della"), Ref("giorno2"))),
        Seq(alt("leggi il calendario|leggimi gli impegni|dimmi gli impegni|cosa ho in agenda"), Opt(Ref("giorno2")))),
    "ora": Alt("che ore sono", u"che ora è", "dimmi l'ora", "sai che ore sono", "mi dici che ore sono"),
    "data": Alt(u"che giorno è oggi", u"che giorno è", u"che data è oggi", u"che giorno è domani",
                u"che data è"),
}
TOP = ["imposta_timer", "stato_timer", "annulla_timer", "imposta_sveglia", "elenca_sveglie", "cancella_sveglia",
       "chiama", "riproduci_musica", "controlla_musica", "apri", "meteo", "leggi_calendario", "ora", "data"]
DYNAMIC = ["contatto", "brano"]


def xml(node):
    if isinstance(node, str):
        return escape(node)
    if isinstance(node, Seq):
        return " ".join(xml(i) for i in node.items)
    if isinstance(node, Opt):
        return '<item repeat="0-1">%s</item>' % xml(node.item)
    if isinstance(node, Ref):
        return '<ruleref uri="#%s"/>' % node.name
    if isinstance(node, Alt):
        return "<one-of>%s</one-of>" % "".join("<item>%s</item>" % xml(i) for i in node.items)
    raise TypeError(node)


def srgs():
    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<grammar version="1.0" xml:lang="it-IT" root="comando" tag-format="semantics/1.0" '
           'xmlns="http://www.w3.org/2001/06/grammar">',
           '<!-- generated by tools/wsp/grammar.py: do not edit -->',
           '<rule id="comando" scope="public"><one-of>%s</one-of></rule>' %
           "".join('<item><ruleref uri="#%s"/><tag>out.intento="%s";</tag></item>' % (t, t) for t in TOP)]
    for name, node in RULES.items():
        if name in DYNAMIC:
            continue
        out.append('<rule id="%s">%s</rule>' % (name, xml(node)))
    out.append("<!--DINAMICO-->")
    out.append("</grammar>")
    return "\n".join(out) + "\n"


def sample(node, rng):
    if isinstance(node, str):
        return node
    if isinstance(node, Seq):
        return " ".join(s for s in (sample(i, rng) for i in node.items) if s)
    if isinstance(node, Opt):
        return sample(node.item, rng) if rng.random() < 0.5 else ""
    if isinstance(node, Ref):
        return sample(RULES[node.name], rng)
    if isinstance(node, Alt):
        return sample(rng.choice(node.items), rng)
    raise TypeError(node)


def samples(n, seed=1):
    rng = random.Random(seed)
    out = []
    for i in range(n):
        top = TOP[i % len(TOP)]
        out.append((sample(RULES[top], rng), top))
    return out


if __name__ == "__main__":
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(srgs())
    print("comandi.grxml written (%d bytes)" % os.path.getsize(OUT))
    for text, intent in samples(20, seed=7):
        print("  %-18s %s" % (intent, text))
