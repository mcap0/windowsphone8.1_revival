#!/usr/bin/env python3
"""parlato.tsv: ~1000 spoken requests as Vosk writes them (giro 8, 2026-11-03), for MEASURING the network.

The phone tests of 2026-11-03 evening showed what free speech looks like after Vosk: no punctuation, long openings
("in pratica vorrei che praticamente adesso..."), verbs in the wrong person ("tu mi mette una sveglia"), chatter
after the request ("per favore lo puoi fare", "porca miseria"), a stray word at the end ("internazionale").
Every piece below (requests with their expected arguments, openings, endings, slips) was written by hand before
looking at what the network does with them. The sentences are combinations of the pieces (seeded, reproducible).

Usage: parlato.py            writes parlato.tsv (same format as golden.tsv, "now" = Tue 2026-11-03 10:00)
"""
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))

# (request, intent, arguments, kind). kind: "free" = has free text at its end (note, reminder, search, artist):
# only polite endings go after it, since anything else would really be part of the text.
REQ = []


def req(intent, kind, *rows):
    for r in rows:
        text, args = (r, "") if isinstance(r, str) else r
        REQ.append((text, intent, args, kind))


req("imposta_timer", "",
    ("mettimi un timer di cinque minuti", "secondi=300"),
    ("mi metti un timer di dieci minuti", "secondi=600"),
    ("mi mette un timer da tre minuti", "secondi=180"),
    ("metti il timer a venti minuti", "secondi=1200"),
    ("imposta un timer di un quarto d'ora", "secondi=900"),
    ("impostare un timer di mezz'ora", "secondi=1800"),
    ("fai partire il timer di otto minuti", "secondi=480"),
    ("un timer di quaranta secondi", "secondi=40"),
    ("timer quindici minuti", "secondi=900"),
    ("avvisami tra dieci minuti", "secondi=600"),
    ("avvisami fra un'ora", "secondi=3600"),
    ("mi serve un timer di sette minuti", "secondi=420"),
    ("fammi un timer di dodici minuti per la pasta", "secondi=720"),
    ("mettere un timer di due minuti", "secondi=120"),
    ("un conto alla rovescia di trenta secondi", "secondi=30"),
    ("mi fai un timer di un'ora e mezza", "secondi=5400"),
    ("metti un timer di quattro minuti per le uova", "secondi=240"),
    ("puoi far partire un timer di venticinque minuti", "secondi=1500"))
req("stato_timer", "",
    "quanto manca al timer",
    "quanto tempo manca al timer",
    "quanto manca alla fine del timer",
    "a che punto è il timer",
    "quanto resta sul timer",
    "il timer quanto manca")
req("annulla_timer", "",
    "ferma il timer",
    "annulla il timer",
    "togli il timer",
    "cancella il timer",
    "il timer non serve più",
    "stoppa il timer")
req("imposta_sveglia", "",
    ("mettimi una sveglia per le cinque", "ora=05:00;giorno="),
    ("mi mette una sveglia per le sette", "ora=07:00;giorno="),
    ("una sveglia per le sei e mezza", "ora=06:30;giorno="),
    ("svegliami domani alle sette", "ora=07:00;giorno=2026-11-04"),
    ("svegliami alle otto meno un quarto", "ora=07:45;giorno="),
    ("metti la sveglia alle sei e un quarto", "ora=06:15;giorno="),
    ("imposta la sveglia per domani mattina alle sei", "ora=06:00;giorno=2026-11-04"),
    ("sveglia alle sette e venti", "ora=07:20;giorno="),
    ("mettere la sveglia alle nove", "ora=09:00;giorno="),
    ("mi svegli alle cinque e quaranta", "ora=05:40;giorno="),
    ("devi svegliarmi alle sei", "ora=06:00;giorno="),
    ("punta la sveglia alle sette e mezza", "ora=07:30;giorno="),
    ("sveglia domani alle otto", "ora=08:00;giorno=2026-11-04"),
    ("metti una sveglia venerdì alle sei", "ora=06:00;giorno=2026-11-06"),
    ("una sveglia alle dieci di sera", "ora=22:00;giorno="),
    ("svegliami dopodomani alle sette", "ora=07:00;giorno=2026-11-05"),
    ("metti la sveglia per le quattro e cinquanta", "ora=04:50;giorno="))
req("elenca_sveglie", "",
    "che sveglie ho",
    "quali sveglie ci sono",
    "ho messo la sveglia",
    "dimmi le sveglie",
    "che sveglie ho messo")
req("cancella_sveglia", "",
    ("cancella la sveglia delle sette", "ora=07:00"),
    ("togli la sveglia delle sei e mezza", "ora=06:30"),
    ("elimina la sveglia delle otto", "ora=08:00"),
    ("annulla la sveglia delle cinque", "ora=05:00"))
req("promemoria", "free",
    ("ricordami alle cinque di chiamare il dottore", "ora=17:00;testo=chiamare il dottore"),
    ("ricordami domani alle nove di pagare la bolletta", "ora=09:00;giorno=2026-11-04;testo=pagare la bolletta"),
    ("ricordami tra mezz'ora di spegnere il forno", "ora=10:30;testo=spegnere il forno"),
    ("ricordami alle sei di comprare il pane", "ora=18:00;testo=comprare il pane"),
    ("mi ricordi alle tre di passare in banca", "ora=15:00;testo=passare in banca"),
    ("ricordami tra un'ora di ritirare il bucato", "ora=11:00;testo=ritirare il bucato"))
req("prendi_nota", "free",
    ("scrivi nelle note pane latte e uova", "testo=pane latte e uova"),
    ("prendi nota che il codice del cancello è quattro due uno", "testo=il codice del cancello è quattro due uno"),
    ("segnati di comprare le lampadine", "testo=comprare le lampadine"),
    ("aggiungi alle note chiamare l'idraulico", "testo=chiamare l'idraulico"),
    ("salva nelle note che devo portare la macchina dal meccanico",
     "testo=devo portare la macchina dal meccanico"),
    ("annota che giovedì c'è la riunione", "testo=giovedì c'è la riunione"))
req("riproduci_musica", "free",
    ("metti i pink floyd", "richiesta=pink floyd"),
    ("riproduci i pink floyd", "richiesta=pink floyd"),
    ("fammi sentire vasco rossi", "richiesta=vasco rossi"),
    ("metti un po' di musica", "tipo=casuale"),
    ("metti della musica", "tipo=casuale"),
    ("voglio ascoltare lucio dalla", "richiesta=lucio dalla"),
    ("metti qualcosa dei queen", "richiesta=queen"),
    ("suona battiato", "richiesta=battiato"),
    ("metti musica a caso", "tipo=casuale"),
    ("fammi ascoltare de andrè", "richiesta=de andrè"),
    ("riproduci l'album the wall", "richiesta=the wall;tipo=album"),
    ("metti su i beatles", "richiesta=beatles"))
req("controlla_musica", "",
    ("metti in pausa", "azione=pausa"),
    ("pausa", "azione=pausa"),
    ("ferma la musica", "azione=pausa"),
    ("stop alla musica", "azione=pausa"),
    ("vai avanti", "azione=avanti"),
    ("passa alla prossima canzone", "azione=avanti"),
    ("salta questa canzone", "azione=avanti"),
    ("cambia canzone", "azione=avanti"),
    ("metti la canzone di prima", "azione=indietro"),
    ("torna alla canzone precedente", "azione=indietro"),
    ("riprendi la musica", "azione=riprendi"),
    ("fai ripartire la musica", "azione=riprendi"),
    ("continua la musica", "azione=riprendi"))
req("chiama", "call",
    ("chiama mia madre", "chi=mia madre"),
    ("chiamare mia madre", "chi=mia madre"),
    ("chiama mamma", "chi=mamma"),
    ("telefona a papà", "chi=papà"),
    ("chiama giuseppe", "chi=giuseppe"),
    ("fai una telefonata a marco", "chi=marco"),
    ("chiamami la nonna", "chi=la nonna"),
    ("fai partire una chiamata a luca", "chi=luca"),
    ("chiama a casa", "chi=casa"),
    ("telefonare a salvo", "chi=salvo"),
    ("devo chiamare francesca", "chi=francesca"),
    ("componi il numero di anna", "chi=anna"))
req("apri", "",
    ("apri il bluetooth", "cosa=bluetooth"),
    ("apri le impostazioni del wifi", "cosa=wi-fi"),
    ("aprimi telegram", "cosa=telegram"),
    ("apri la posta", "cosa=posta"),
    ("apri il registratore", "cosa=registratore"),
    ("attiva il bluetooth", "cosa=bluetooth"),
    ("vai nelle impostazioni del bluetooth", "cosa=bluetooth"),
    ("apri il calendario", "cosa=calendario"),
    ("aprire l'orologio", "cosa=orologio"),
    ("apri la modalità aereo", "cosa=modalità aereo"))
req("cerca_sul_web", "free",
    ("cerca su internet la ricetta della caponata", "domanda=la ricetta della caponata"),
    ("cerca su google gli orari del treno per messina", "domanda=gli orari del treno per messina"),
    ("cercami la farmacia di turno", "domanda=la farmacia di turno"),
    ("cerca come si pulisce il forno", "domanda=come si pulisce il forno"))
req("meteo", "",
    ("che tempo fa a catania", "luogo=catania;giorno="),
    ("che tempo fa a palermo", "luogo=palermo;giorno="),
    ("che tempo farà domani a palermo", "luogo=palermo;giorno=2026-11-04"),
    ("com'è il tempo a catania", "luogo=catania;giorno="),
    ("come è il tempo a catania", "luogo=catania;giorno="),
    ("il meteo di messina", "luogo=messina;giorno="),
    ("che tempo fa", "luogo=;giorno="),
    ("che tempo fa domani", "luogo=;giorno=2026-11-04"),
    ("piove domani a roma", "luogo=roma;giorno=2026-11-04"),
    ("le previsioni del tempo per domani", "luogo=;giorno=2026-11-04"),
    ("quanti gradi ci sono a milano", "luogo=milano;giorno="),
    ("devo prendere l'ombrello", "luogo=;giorno="),
    ("fa freddo fuori", "luogo=;giorno="),
    ("dimmi il meteo di sabato a siracusa", "luogo=siracusa;giorno=2026-11-07"),
    ("che tempo fa oggi a ragusa", "luogo=ragusa;giorno="),
    ("che tempo ci sarà dopodomani a napoli", "luogo=napoli;giorno=2026-11-05"))
req("leggi_calendario", "",
    ("cosa ho domani", "dal=2026-11-04;giorni=1"),
    ("che impegni ho oggi", "dal=2026-11-03;giorni=1"),
    ("cosa c'è in agenda domani", "dal=2026-11-04;giorni=1"),
    ("leggimi il calendario di oggi", "dal=2026-11-03;giorni=1"),
    ("ho appuntamenti domani", "dal=2026-11-04;giorni=1"),
    ("che impegni ho questa settimana", "dal=2026-11-03;giorni=7"),
    ("sono libero venerdì", "dal=2026-11-06;giorni=1"),
    ("cosa devo fare oggi", "dal=2026-11-03;giorni=1"))
req("ora", "",
    "che ore sono",
    "che ora è",
    "mi dici che ore sono",
    "sai che ore sono",
    "dimmi l'ora")
req("data", "",
    "che giorno è oggi",
    "quanti ne abbiamo oggi",
    "che giorno è",
    "dimmi la data di oggi",
    "che data è oggi")
# for Gemini: questions and chatter that only look like commands
req("online", "free",
    "chi ha vinto la partita ieri sera",
    "come funziona la fotosintesi",
    "spiegami come si fa la pasta alla norma",
    "chi è il presidente della repubblica",
    "come si chiama il cantante dei pink floyd",
    "quanto è alto l'etna",
    "che cos'è un timer",
    "come si imposta una sveglia",
    "raccontami una barzelletta",
    "traduci buongiorno in inglese",
    "quanto fa sette per otto",
    "perché il cielo è blu",
    "che canzone è questa",
    "chi canta questa canzone",
    "il meteo dice sempre che piove",
    "mia madre mi chiama sempre alle otto",
    "la sveglia stamattina non ha suonato",
    "ieri ho ascoltato i pink floyd tutto il giorno",
    "scrivimi una poesia sul mare",
    "dove si trova la farmacia più vicina",
    "come sta andando la borsa oggi",
    "quanti abitanti ha catania",
    "consigliami un film da vedere stasera",
    "che differenza c'è tra un virus e un batterio",
    "fammi un riassunto dei promessi sposi",
    "quando è nato garibaldi",
    "mi spieghi cosa vuol dire resilienza",
    "come si dice gatto in francese")

# openings people said before the request (phone log 2026-11-03 19:44-19:55 and the like)
PRE = [
    "", "", "", "", "",
    "allora", "senti", "ascolta", "ehi", "allora senti", "dunque", "ehm", "eh", "cioè", "niente", "ok",
    "in pratica", "praticamente", "in pratica vorrei che praticamente adesso", "adesso", "ora", "ascoltami",
    "vorrei che tu", "voglio che tu", "ho bisogno che tu", "per favore", "per piacere", "scusa", "scusami",
    "mi faresti un favore", "posso chiederti una cosa", "senti un attimo", "aspetta", "guarda",
    "allora praticamente", "in pratica vorrei", "volevo chiederti", "ti volevo chiedere", "ciao", "buongiorno",
    "ehi assistente", "ok allora", "allora vediamo", "dai", "sì allora", "no aspetta",
    "senti una cosa", "mi raccomando", "fammi una cortesia", "per cortesia", "allora adesso", "va bene allora",
    "ehm allora", "eh niente", "senti scusa", "allora in pratica", "bene", "ok senti",
]
# endings: polite ones go anywhere, the rest only after requests without free text at the end
POLITE = ["per favore", "grazie", "grazie mille", "per piacere", "per cortesia"]
TAIL = [
    "lo puoi fare", "potresti farlo", "se puoi", "se ci riesci", "porca miseria", "mannaggia", "dai",
    "subito", "va bene", "ok", "grazie tante", "che devo uscire", "per favore lo puoi fare", "per favore grazie",
    "che sono di fretta", "ti ringrazio", "va bene così", "intanto", "così vedo", "se non ti dispiace",
    "per favore lo puoi fare internazionale", "per favore la specifica", "porca miseria", "mannaggia la miseria",
    "così via", "e niente", "grazie amico mio", "dai su", "e basta", "adesso",
]
# the request asked for, not ordered: "mi puoi ...", "potresti ..." (the verb stays as said)
ASK = ["mi puoi", "puoi", "potresti", "mi potresti", "riesci a", "sapresti", "vorresti", "ti dispiace"]
INF = {"mettimi": "mettermi", "metti": "mettere", "imposta": "impostare", "chiama": "chiamare", "apri": "aprire",
       "aprimi": "aprirmi", "svegliami": "svegliarmi", "ricordami": "ricordarmi", "riproduci": "riprodurre",
       "cancella": "cancellare", "ferma": "fermare", "cerca": "cercare", "cercami": "cercarmi", "scrivi": "scrivere",
       "togli": "togliere", "annulla": "annullare", "fammi": "farmi", "dimmi": "dirmi", "passa": "passare",
       "elimina": "eliminare", "telefona": "telefonare", "segnati": "segnarti", "salva": "salvare",
       "aggiungi": "aggiungere", "annota": "annotare", "suona": "suonare", "fai": "fare", "avvisami": "avvisarmi",
       "attiva": "attivare", "vai": "andare", "leggimi": "leggermi", "riprendi": "riprendere", "torna": "tornare",
       "salta": "saltare", "cambia": "cambiare", "stoppa": "stoppare", "punta": "puntare", "chiamami": "chiamarmi",
       "componi": "comporre", "continua": "continuare", "prendi": "prendere"}
# Vosk slips: the verb in another person or the infinitive (whole words only)
SLIPS = {
    "mettimi": ["mi mette", "mettere", "mi metti"], "metti": ["mette", "mettere"], "imposta": ["impostare", "imposti"],
    "chiama": ["chiamare", "chiami", "chiamo"], "apri": ["aprire", "apre"], "svegliami": ["mi sveglia", "svegliarmi"],
    "ricordami": ["mi ricorda", "ricordarmi"], "riproduci": ["riprodurre"], "cancella": ["cancellare"],
    "ferma": ["fermare"], "cerca": ["cercare"], "scrivi": ["scrivere"],
}


def slip(rng, text):
    w = text.split()
    for i, x in enumerate(w):
        if x in SLIPS and rng.random() < 0.5:
            w[i] = rng.choice(SLIPS[x])
            break
    return " ".join(w)


def sentence(rng, text, intent, kind):
    """one spoken sentence around a request"""
    pre = rng.choice(PRE)
    if kind == "free" or intent == "online":
        end = rng.choice(POLITE + [""] * 4)
    elif kind == "call":
        end = rng.choice(POLITE + ["lo puoi fare", "potresti farlo", "subito", "se puoi", "per favore lo puoi fare",
                                   "adesso", "dai", "potresti farlo per favore"] + [""] * 3)
    else:
        end = rng.choice(POLITE + TAIL + [""] * 6)
    r = rng.random()
    first = text.split()[0]
    if intent != "online" and r < 0.2 and (first in INF or first in ("un", "una")):
        rest = text[len(first):]
        text = rng.choice(ASK) + " " + (INF[first] + rest if first in INF else "mettere " + text)
    elif r < 0.45:
        text = slip(rng, text)
    return " ".join(p for p in (pre, text, end) if p)


def main():
    rng = random.Random(20261103)
    out = ["# Parlato (giro 8, 2026-11-03): ~1000 richieste a voce come le scrive Vosk, generate da parlato.py con",
           "# pezzi scritti a mano PRIMA di misurare (aperture, code, verbi sbagliati). \"now\" = mar 2026-11-03 10:00."]
    seen = set()
    # every request at least twice, then the common intents more often (as people use them)
    weight = {"imposta_timer": 3, "imposta_sveglia": 3, "meteo": 3, "chiama": 3, "riproduci_musica": 2,
              "controlla_musica": 2, "online": 2}
    pool = [x for x in REQ for _ in range(weight.get(x[1], 1))]
    order = [x for x in REQ for _ in range(2)]
    while len(out) - 2 < 1000:
        text, intent, args, kind = order.pop(0) if order else rng.choice(pool)
        s = sentence(rng, text, intent, kind)
        if s in seen:
            continue
        seen.add(s)
        out.append("\t".join(x for x in (s, intent, args) if x) if args else s + "\t" + intent)
    with open(os.path.join(HERE, "parlato.tsv"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("parlato.tsv: %d frasi" % (len(out) - 2))


if __name__ == "__main__":
    main()
