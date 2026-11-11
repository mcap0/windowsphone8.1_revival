#!/usr/bin/env python3
"""cortesi-a.tsv, cortesi-b.tsv: polite spoken requests (giro 10, 2026-11-03 notte), for MEASURING the network.

The phone tests of 2026-11-03 22:00-22:25 (Vosk live) asked everything politely and indirectly:
"caro assistente mi piacerebbe se tu riuscissi a mettere una sveglia per le diciassette gentilmente",
"ciao amico mio assistente a me piacerebbe molto se tu mi potessi un attimo cercare su internet che cosa fa la roma",
"dovresti farmi gentilmente il favore di mettere una nota su onenote scrivi ...". New against parlato.py: an address
first ("caro assistente", misheard "il mio agente"), a polite frame that puts the verb in the conditional
("metteresti", "cercheresti") or the imperfect subjunctive ("mi mettessi", "mi dicessi"), fillers inside the frame
("per favore", "gentilmente", "un attimo"), Vosk echoes ("diresti resti", "potresti certi").

Two sets with DIFFERENT pieces (addresses, frames, fillers, endings, requests), all written by hand before measuring:
  A (cortesi-a.tsv) is the one the weights get fixed on; B (cortesi-b.tsv) is only measured after, to see whether the
  fix holds for sentences it was not made on. C (cortesi-c.tsv, giro 11) was written before the structural fixes of
  giro 11 and is only measured at their end.

Usage: cortesi.py      writes both (golden.tsv format, "now" = Tue 2026-11-03 10:00)
"""
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))

# verb: imperative, infinitive, conditional (tu), imperfect subjunctive (tu)
VERBS = {
    "mettere": ("metti", "mettere", "metteresti", "mettessi"),
    "impostare": ("imposta", "impostare", "imposteresti", "impostassi"),
    "far partire": ("fai partire", "far partire", "faresti partire", "facessi partire"),
    "chiamare": ("chiama", "chiamare", "chiameresti", "chiamassi"),
    "telefonare": ("telefona", "telefonare", "telefoneresti", "telefonassi"),
    "cercare": ("cerca", "cercare", "cercheresti", "cercassi"),
    "dire": ("di", "dire", "diresti", "dicessi"),
    "fare": ("fai", "fare", "faresti", "facessi"),
    "svegliare": ("sveglia", "svegliare", "sveglieresti", "svegliassi"),
    "ricordare": ("ricorda", "ricordare", "ricorderesti", "ricordassi"),
    "scrivere": ("scrivi", "scrivere", "scriveresti", "scrivessi"),
    "salvare": ("salva", "salvare", "salveresti", "salvassi"),
    "riprodurre": ("riproduci", "riprodurre", "riprodurresti", "riproducessi"),
    "leggere": ("leggi", "leggere", "leggeresti", "leggessi"),
    "spiegare": ("spiega", "spiegare", "spiegheresti", "spiegassi"),
    "raccontare": ("racconta", "raccontare", "racconteresti", "raccontassi"),
    "controllare": ("controlla", "controllare", "controlleresti", "controllassi"),
    "segnare": ("segna", "segnare", "segneresti", "segnassi"),
    "consigliare": ("consiglia", "consigliare", "consiglieresti", "consigliassi"),
    "annullare": ("annulla", "annullare", "annulleresti", "annullassi"),
}
IMP_MI = {"di": "dimmi", "fai": "fammi", "metti": "mettimi", "sveglia": "svegliami", "ricorda": "ricordami",
          "cerca": "cercami", "spiega": "spiegami", "racconta": "raccontami", "leggi": "leggimi",
          "consiglia": "consigliami", "fai partire": "fammi partire", "segna": "segnami", "imposta": "impostami",
          "controlla": "controllami", "salva": "salvami", "scrivi": "scrivimi"}


def conj(verb, form, mi):
    imp, inf, cond, subj = VERBS[verb]
    if form == "imp":
        return IMP_MI.get(imp, imp + "mi") if mi else imp
    if form == "inf":
        if not mi:
            return inf
        head, _, rest = inf.partition(" ")
        return head[:-1] + "mi" + (" " + rest if rest else "")  # mettere -> mettermi, far partire -> farmi partire
    return ("mi " if mi else "") + (cond if form == "cond" else subj)


# a request: (verb, "mi" clitic, the rest, intent, arguments, kind); kind "free" = free text at its end
def R(verb, mi, rest, intent, args="", kind=""):
    return (verb, mi, rest, intent, args, kind)


SETS = {
    "a": dict(
        address=["caro assistente", "mio caro assistente", "ciao amico mio assistente", "gentilissimo assistente",
                 "ciao caro", "amico mio assistente", "il mio agente", "ciao amico assistente", "caro mio assistente",
                 "", ""],
        # (verb form, template): {v} the verb, {f} an optional filler
        frames=[("inf", "potresti {f}{v}"), ("inf", "mi potresti {f}{v}"), ("inf", "riusciresti {f}a {v}"),
                ("inf", "non è che riusciresti a {v}"), ("inf", "dovresti farmi {f}il favore di {v}"),
                ("inf", "mi faresti il favore di {v}"), ("subj", "mi piacerebbe se tu {f}{v}"),
                ("subj", "mi piacerebbe molto se tu {v}"), ("inf", "mi piacerebbe se tu riuscissi a {v}"),
                ("inf", "a me piacerebbe molto se tu potessi {f}{v}"), ("cond", "{v}"), ("cond", "{f}{v}"),
                ("inf", "vorrei che tu riuscissi a {v}"), ("imp", "{v}")],
        fillers=["gentilmente ", "per favore ", "un attimo ", "adesso ", "in questo momento "],
        endings=["grazie", "gentilmente", "se puoi per favore", "per favore", "ti ringrazio", "grazie mille"],
        free_endings=["per favore", "grazie", "grazie mille"],
        echoes=[("diresti", "diresti resti"), ("potresti", "potresti certi"), ("riusciresti", "riusciresti noi"),
                ("mettere", "mettere e"), ("piacerebbe", "piacerebbe be")],
        requests=[
            R("mettere", False, "un timer di cinque minuti", "imposta_timer", "secondi=300"),
            R("mettere", False, "un timer di dieci secondi", "imposta_timer", "secondi=10"),
            R("impostare", False, "un timer di venti minuti", "imposta_timer", "secondi=1200"),
            R("far partire", False, "un timer di tre minuti", "imposta_timer", "secondi=180"),
            R("mettere", False, "un timer di un quarto d'ora", "imposta_timer", "secondi=900"),
            R("dire", True, "a che punto è il timer", "stato_timer"),
            R("dire", True, "quanto manca al timer", "stato_timer"),
            R("mettere", False, "una sveglia per le diciassette", "imposta_sveglia", "ora=17:00;etichetta="),
            R("mettere", False, "una sveglia alle quindici", "imposta_sveglia", "ora=15:00;etichetta="),
            R("impostare", False, "una sveglia per le sette e mezza", "imposta_sveglia", "ora=07:30;etichetta="),
            R("mettere", False, "una sveglia domani alle sei", "imposta_sveglia",
              "ora=06:00;giorno=2026-11-04;etichetta="),
            R("svegliare", True, "alle otto", "imposta_sveglia", "ora=08:00;etichetta="),
            R("svegliare", True, "domani alle sette", "imposta_sveglia", "ora=07:00;giorno=2026-11-04;etichetta="),
            R("dire", True, "che tempo fa a catania", "meteo", "luogo=catania;giorno="),
            R("dire", True, "che tempo farà domani a palermo", "meteo", "luogo=palermo;giorno=2026-11-04"),
            R("dire", True, "come è il tempo a messina", "meteo", "luogo=messina;giorno="),
            R("dire", True, "se domani piove a roma", "meteo", "luogo=roma;giorno=2026-11-04"),
            R("chiamare", False, "mamma", "chiama", "chi=mamma"),
            R("chiamare", False, "giuseppe", "chiama", "chi=giuseppe"),
            R("telefonare", False, "a papà", "chiama", "chi=papà"),
            R("mettere", False, "i pink floyd", "riproduci_musica", "richiesta=pink floyd"),
            R("mettere", False, "i pink floyd in sottofondo", "riproduci_musica", "richiesta=pink floyd"),
            R("riprodurre", False, "qualcosa dei queen", "riproduci_musica", "richiesta=queen"),
            R("cercare", False, "qualche canzone di vasco rossi", "riproduci_musica", "richiesta=vasco rossi"),
            R("cercare", False, "su internet cosa fa la roma la prossima settimana", "cerca_sul_web",
              "domanda=cosa fa la roma la prossima settimana", "free"),
            R("cercare", False, "su internet a che ora c'è l'eclissi domani", "cerca_sul_web",
              "domanda=a che ora c'è l'eclissi domani", "free"),
            R("fare", True, "una ricerca su internet su come si fa un foglio excel", "cerca_sul_web",
              "domanda=come si fa un foglio excel", "free"),
            R("cercare", False, "su google gli orari dei treni per messina", "cerca_sul_web",
              "domanda=gli orari dei treni per messina", "free"),
            R("salvare", False, "una nota con scritto domani devo comprare il latte", "prendi_nota",
              "testo=domani devo comprare il latte", "free"),
            R("mettere", False, "una nota su onenote e scrivere chiamare l'idraulico", "prendi_nota",
              "testo=chiamare l'idraulico", "free"),
            R("mettere", False, "una nota sua notte e scrivere il codice del cancello è quattro due uno",
              "prendi_nota", "testo=il codice del cancello è quattro due uno", "free"),
            R("scrivere", False, "nelle note comprare le lampadine", "prendi_nota", "testo=comprare le lampadine",
              "free"),
            R("ricordare", True, "domani alle nove di pagare la bolletta", "promemoria",
              "ora=09:00;giorno=2026-11-04;testo=pagare la bolletta", "free"),
            R("ricordare", True, "alle cinque di chiamare il dottore", "promemoria",
              "ora=17:00;testo=chiamare il dottore", "free"),
            R("dire", True, "che ore sono", "ora"),
            R("dire", True, "che giorno è oggi", "data"),
            R("dire", True, "cosa ho in agenda domani", "leggi_calendario", "dal=2026-11-04;giorni=1"),
            R("leggere", True, "gli impegni di oggi", "leggi_calendario", "dal=2026-11-03;giorni=1"),
            R("dire", True, "chi ha vinto la partita ieri sera", "online", "", "free"),
            R("dire", True, "che cosa fa la roma la prossima settimana", "online", "", "free"),
            R("dire", True, "a che ora c'è natale nel duemilaventisette", "online", "", "free"),
            R("spiegare", True, "come si fa la pasta alla norma", "online", "", "free"),
            R("raccontare", True, "una barzelletta", "online", "", "free"),
            R("dire", True, "come funziona la fotosintesi", "online", "", "free"),
        ]),
    "b": dict(
        address=["assistente carissimo", "ciao assistente mio", "ehi assistente caro", "senti caro", "carissimo",
                 "assistente bello", "mio agente caro", "buonasera assistente", "ciao tesoro", "", ""],
        frames=[("inf", "saresti così gentile da {v}"), ("inf", "ti dispiacerebbe {f}{v}"),
                ("subj", "vorrei tanto che tu {f}{v}"), ("inf", "sarebbe possibile {v}"),
                ("inf", "avrei bisogno che tu riuscissi a {v}"), ("cond", "per caso {v}"),
                ("inf", "ti andrebbe di {f}{v}"), ("inf", "mi faresti la cortesia di {v}"),
                ("subj", "sarei contento se tu {v}"), ("inf", "puoi {f}{v}"),
                ("inf", "se non ti dispiace potresti {v}"), ("imp", "{v} {f}"), ("cond", "non è che {v}"),
                ("inf", "ti chiederei di {f}{v}")],
        fillers=["cortesemente ", "per piacere ", "un secondo ", "quando puoi ", "velocemente "],
        endings=["grazie davvero", "sei gentilissimo", "se ci riesci", "ti ringrazio tanto", "molto gentile",
                 "per cortesia"],
        free_endings=["per cortesia", "ti ringrazio", "grazie davvero"],
        echoes=[("ti", "ti di"), ("vorrei", "vorrei rei"), ("saresti", "saresti esti"), ("sarebbe", "sarebbe be"),
                ("puoi", "puoi poi")],
        requests=[
            R("mettere", False, "un timer di sette minuti per la pasta", "imposta_timer", "secondi=420"),
            R("far partire", False, "un conto alla rovescia di trenta secondi", "imposta_timer", "secondi=30"),
            R("impostare", False, "un timer di un'ora", "imposta_timer", "secondi=3600"),
            R("mettere", False, "un timer di quaranta minuti", "imposta_timer", "secondi=2400"),
            R("controllare", False, "quanto manca alla fine del timer", "stato_timer"),
            R("dire", True, "quanto resta sul timer", "stato_timer"),
            R("mettere", False, "una sveglia alle sei e un quarto", "imposta_sveglia", "ora=06:15;etichetta="),
            R("impostare", False, "una sveglia per le nove di sera", "imposta_sveglia", "ora=21:00;etichetta="),
            R("mettere", False, "una sveglia per domani alle otto", "imposta_sveglia",
              "ora=08:00;giorno=2026-11-04;etichetta="),
            R("svegliare", True, "alle cinque", "imposta_sveglia", "ora=05:00;etichetta="),
            R("impostare", False, "una sveglia alle dieci meno un quarto", "imposta_sveglia", "ora=09:45;etichetta="),
            R("dire", True, "che tempo fa a siracusa", "meteo", "luogo=siracusa;giorno="),
            R("dire", True, "quanti gradi ci sono a milano", "meteo", "luogo=milano;giorno="),
            R("dire", True, "che tempo ci sarà dopodomani a napoli", "meteo", "luogo=napoli;giorno=2026-11-05"),
            R("dire", True, "che tempo fa oggi a ragusa", "meteo", "luogo=ragusa;giorno="),
            R("chiamare", False, "papà", "chiama", "chi=papà"),
            R("chiamare", False, "marco", "chiama", "chi=marco"),
            R("telefonare", False, "a mamma", "chiama", "chi=mamma"),
            R("mettere", False, "vasco rossi", "riproduci_musica", "richiesta=vasco rossi"),
            R("riprodurre", False, "l'album the wall", "riproduci_musica", "richiesta=the wall;tipo=album"),
            R("mettere", False, "qualcosa di lucio dalla", "riproduci_musica", "richiesta=lucio dalla"),
            R("mettere", False, "un po' di battiato", "riproduci_musica", "richiesta=battiato"),
            R("cercare", False, "su google la farmacia di turno", "cerca_sul_web", "domanda=la farmacia di turno",
              "free"),
            R("cercare", False, "su internet quando gioca il napoli", "cerca_sul_web",
              "domanda=quando gioca il napoli", "free"),
            R("cercare", False, "su internet come si pulisce il forno", "cerca_sul_web",
              "domanda=come si pulisce il forno", "free"),
            R("fare", True, "una ricerca su internet sulla ricetta della caponata", "cerca_sul_web",
              "domanda=la ricetta della caponata", "free"),
            R("segnare", False, "nelle note che giovedì c'è la riunione", "prendi_nota",
              "testo=giovedì c'è la riunione", "free"),
            R("scrivere", False, "nelle note pane latte e uova", "prendi_nota", "testo=pane latte e uova", "free"),
            R("salvare", False, "una nota con scritto portare la macchina dal meccanico", "prendi_nota",
              "testo=portare la macchina dal meccanico", "free"),
            R("ricordare", True, "tra un'ora di ritirare il bucato", "promemoria",
              "ora=11:00;testo=ritirare il bucato", "free"),
            R("ricordare", True, "alle sei di comprare il pane", "promemoria", "ora=18:00;testo=comprare il pane",
              "free"),
            R("dire", True, "l'ora", "ora"),
            R("dire", True, "che ora è", "ora"),
            R("dire", True, "la data di oggi", "data"),
            R("dire", True, "che impegni ho questa settimana", "leggi_calendario", "dal=2026-11-03;giorni=7"),
            R("dire", True, "chi è il presidente della repubblica", "online", "", "free"),
            R("dire", True, "quanto è alto l'etna", "online", "", "free"),
            R("spiegare", True, "perché il cielo è blu", "online", "", "free"),
            R("consigliare", True, "un film da vedere stasera", "online", "", "free"),
            R("dire", True, "quando è nato garibaldi", "online", "", "free"),
        ]),
    # C (giro 11, written before the structural fixes, measured only at the end): other frames, addresses, endings
    "c": dict(
        address=["ascolta assistente", "ciao bello mio", "senti amico", "ehi tu", "caro il mio assistente",
                 "assistente mio adorato", "buongiorno caro", "ciao assistente gentile", "dimmi un po'", "", ""],
        frames=[("inf", "ti sarei grato se potessi {v}"), ("inf", "potrei chiederti di {f}{v}"),
                ("inf", "sei in grado di {v}"), ("subj", "magari se tu {f}{v}"), ("inf", "mi aiuteresti a {v}"),
                ("cond", "quando hai un attimo {v}"), ("inf", "ce la faresti a {f}{v}"),
                ("inf", "mi farebbe piacere che tu riuscissi a {v}"), ("subj", "mi andrebbe che tu {v}"),
                ("inf", "ho voglia di {v}"), ("imp", "{f}{v}"), ("cond", "tu {v}"),
                ("inf", "ti va di {v}"), ("inf", "sapresti {f}{v}")],
        fillers=["pure ", "per gentilezza ", "giusto un attimo ", "per piacere ", "se riesci "],
        endings=["ti ringrazio di cuore", "grazie infinite", "sei un tesoro", "quando puoi", "con calma",
                 "grazie amico"],
        free_endings=["grazie infinite", "ti ringrazio di cuore", "grazie amico"],
        echoes=[("potrei", "potrei rei"), ("faresti", "faresti esti"), ("sapresti", "sapresti presti"),
                ("aiuteresti", "aiuteresti teresti"), ("grado", "grado do")],
        requests=[
            R("mettere", False, "un timer di sei minuti", "imposta_timer", "secondi=360"),
            R("impostare", False, "un timer di due minuti e mezzo", "imposta_timer", "secondi=150"),
            R("far partire", False, "un timer di quindici secondi", "imposta_timer", "secondi=15"),
            R("dire", True, "quanto tempo manca al timer", "stato_timer"),
            R("mettere", False, "una sveglia per le sei e mezza", "imposta_sveglia", "ora=06:30;etichetta="),
            R("impostare", False, "una sveglia alle undici", "imposta_sveglia", "ora=11:00;etichetta="),
            R("mettere", False, "una sveglia per venerdì alle sette", "imposta_sveglia",
              "ora=07:00;giorno=2026-11-06;etichetta="),
            R("svegliare", True, "domani alle sei e un quarto", "imposta_sveglia",
              "ora=06:15;giorno=2026-11-04;etichetta="),
            R("dire", True, "che tempo fa a trapani", "meteo", "luogo=trapani;giorno="),
            R("dire", True, "se domani fa freddo a enna", "meteo", "luogo=enna;giorno=2026-11-04"),
            R("dire", True, "che tempo farà sabato a bologna", "meteo", "luogo=bologna;giorno=2026-11-07"),
            R("chiamare", False, "la nonna", "chiama", "chi=nonna"),
            R("chiamare", False, "francesca", "chiama", "chi=francesca"),
            R("telefonare", False, "a luca", "chiama", "chi=luca"),
            R("mettere", False, "un po' di de andrè", "riproduci_musica", "richiesta=de andrè"),
            R("riprodurre", False, "i beatles", "riproduci_musica", "richiesta=beatles"),
            R("mettere", False, "qualcosa dei coldplay", "riproduci_musica", "richiesta=coldplay"),
            R("cercare", False, "su internet il prezzo della benzina oggi", "cerca_sul_web",
              "domanda=il prezzo della benzina oggi", "free"),
            R("cercare", False, "su google come si cucina il cous cous", "cerca_sul_web",
              "domanda=come si cucina il cous cous", "free"),
            R("fare", True, "una ricerca su internet sugli orari del museo", "cerca_sul_web",
              "domanda=gli orari del museo", "free"),
            R("scrivere", False, "nelle note di pagare l'assicurazione", "prendi_nota",
              "testo=pagare l'assicurazione", "free"),
            R("salvare", False, "una nota con scritto il wifi di casa è gatto nero", "prendi_nota",
              "testo=il wifi di casa è gatto nero", "free"),
            R("ricordare", True, "alle quattro di chiamare l'avvocato", "promemoria",
              "ora=16:00;testo=chiamare l'avvocato", "free"),
            R("ricordare", True, "domani alle dieci di portare il pacco", "promemoria",
              "ora=10:00;giorno=2026-11-04;testo=portare il pacco", "free"),
            R("dire", True, "che ore sono adesso", "ora"),
            R("dire", True, "che giorno è domani", "data"),
            R("dire", True, "cosa ho in programma domani", "leggi_calendario", "dal=2026-11-04;giorni=1"),
            R("dire", True, "chi ha scritto i promessi sposi", "online", "", "free"),
            R("spiegare", True, "come funziona un motore elettrico", "online", "", "free"),
            R("dire", True, "quanti abitanti ha palermo", "online", "", "free"),
            R("raccontare", True, "una storia divertente", "online", "", "free"),
        ]),
}


def sentence(rng, s, req):
    verb, mi, rest, intent, args, kind = req
    form, frame = rng.choice(s["frames"])
    filler = rng.choice(s["fillers"]) if "{f}" in frame and rng.random() < 0.5 else ""
    core = frame.replace("{f}", filler).replace("{v}", conj(verb, form, mi) + " " + rest).strip()
    if rng.random() < 0.25:  # a Vosk echo of a frame word
        for word, echo in rng.sample(s["echoes"], len(s["echoes"])):
            if (" " + core + " ").find(" " + word + " ") >= 0:
                core = (" " + core + " ").replace(" " + word + " ", " " + echo + " ", 1).strip()
                break
    end = rng.choice((s["free_endings"] if kind == "free" else s["endings"]) + [""] * 3)
    return " ".join(p for p in (rng.choice(s["address"]), core, end) if p)


def write(name, s, seed):
    rng = random.Random(seed)
    out = ["# Cortesi set %s (giro 10, 2026-11-03 notte): richieste gentili e indirette come nelle prove sul telefono" %
           name.upper(),
           "# delle 22:00-22:25, da cortesi.py (pezzi scritti a mano prima di misurare). \"now\" = mar 2026-11-03 10:00."]
    seen = set()
    order = [r for r in s["requests"] for _ in range(3)]
    rng.shuffle(order)
    while len(out) - 2 < 1000:
        req = order.pop() if order else rng.choice(s["requests"])
        text = sentence(rng, s, req)
        if text in seen:
            continue
        seen.add(text)
        out.append(text + "\t" + req[3] + ("\t" + req[4] if req[4] else ""))
    path = os.path.join(HERE, "cortesi-%s.tsv" % name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("%s: %d frasi" % (os.path.basename(path), len(out) - 2))


def main():
    write("a", SETS["a"], 20261103)
    write("b", SETS["b"], 20261104)
    write("c", SETS["c"], 20261105)


if __name__ == "__main__":
    main()
