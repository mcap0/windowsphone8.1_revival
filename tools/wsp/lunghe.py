#!/usr/bin/env python3
"""lunghe.tsv: long spoken sentences (giro 9, 2026-11-03), for MEASURING: chatter, then the request at the end.

User's example: "mbare senti ma in pratica io ti vorrei chiedere come va la vita però giusto il punto è che devi
chiamare mia madre puoi chiamare mamma per favore". No public Italian dataset has these, so they are put together
from real pieces: questions people wrote in Italian (OpenAssistant oasst1, CC BY 4.0), commands from MASSIVE it-IT
dev (CC BY 4.0) and from parlato.py, joined by spoken connectors written by hand. One third have no command at all
(chatter + another question): those must end on Google. Written as Vosk does: lowercase, no punctuation.

Usage: lunghe.py <massive it-IT.jsonl> <oasst messages.jsonl.gz> [out.tsv]   (the data is not in the repository)
"""
import gzip, json, os, random, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import parlato

OK = {"alarm_set": "imposta_sveglia|imposta_timer", "alarm_query": "elenca_sveglie|stato_timer",
      "alarm_remove": "cancella_sveglia|annulla_timer", "play_music": "riproduci_musica|controlla_musica",
      "weather_query": "meteo", "datetime_query": "ora|data", "calendar_query": "leggi_calendario"}
OPEN = ["", "", "mbare senti", "senti", "allora", "ma in pratica io ti vorrei chiedere", "ciao", "ascolta",
        "senti una cosa", "allora praticamente", "ehi", "volevo chiederti", "eh niente", "senti ma", "dunque",
        "ascoltami bene", "allora guarda", "senti un attimo", "oh", "ok allora"]
JOIN = ["però giusto il punto è che", "comunque", "ma lasciamo stare", "a parte questo", "ad ogni modo",
        "comunque sia", "e niente", "detto questo", "tornando a noi", "ma la cosa importante è che",
        "il motivo per cui ti chiamo è che", "quindi", "e poi", "anyway", "ma vabbè non importa", "ma senti",
        "ah e un'altra cosa", "va bene lasciamo perdere", "però adesso", "ma prima di tutto", "intanto",
        "ma la cosa che mi serve adesso è che", "però ora", "in ogni caso", "ma dicevo"]
DUTY = {"chiama": "devi chiamare", "metti": "devi mettere", "imposta": "devi impostare", "svegliami": "devi svegliarmi",
        "apri": "devi aprire", "ricordami": "devi ricordarmi"}
END = ["", "", "", "per favore", "grazie", "puoi farlo per favore", "grazie mille", "dai", "se puoi"]


def vosk(text):
    """as Vosk writes it: lowercase, no punctuation, no newlines"""
    t = re.sub(r"[^\w'àèéìòóù ]+", " ", text.lower().replace("\n", " "))
    return re.sub(r"\s+", " ", t).strip()


def main():
    rng = random.Random(20261103)
    massive = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
    cmds = [(vosk(r["utt"]), OK[r["intent"]], "") for r in massive if r["partition"] == "dev" and r["intent"] in OK]
    cmds += [(vosk(t), i, a) for t, i, a, k in parlato.REQ if i != "online"]
    asks = [vosk(r["utt"]) for r in massive if r["partition"] == "dev" and r["intent"].startswith(("qa_", "general_",
            "recommendation_", "cooking_", "news_"))]
    chat = []
    for l in gzip.open(sys.argv[2], "rt", encoding="utf-8"):
        m = json.loads(l)
        if m.get("lang") != "it" or m["role"] != "prompter":
            continue
        first = re.split(r"(?<=[.?!])\s", m["text"].strip())[0]  # one sentence of it
        w = vosk(first).split()
        if 5 <= len(w) <= 30:
            chat.append(" ".join(w))
    out = ["# Lunghe (giro 9, 2026-11-03): chiacchiere vere (oasst1 it) + connettori + comando (MASSIVE it dev,",
           "# parlato.py) o nessun comando (-> Google). Generate da lunghe.py; \"now\" = mar 2026-11-03 10:00."]
    seen = set()
    while len(out) - 2 < 3000:
        pre = " ".join(x for x in (rng.choice(OPEN), rng.choice(chat)) if x)
        if rng.random() < 0.33:  # no command: chatter and another question
            s = " ".join((pre, rng.choice(JOIN), rng.choice(asks + chat), rng.choice(END))).strip()
            row = (s, "online|cerca_sul_web", "")
        else:
            text, intent, args = rng.choice(cmds)
            first = text.split()[0]
            if first in DUTY and rng.random() < 0.4:  # "devi chiamare mia madre"
                text = DUTY[first] + text[len(first):]
            if rng.random() < 0.3:  # said twice, as people do: "devi chiamare mia madre puoi chiamare mamma"
                text = text + " " + rng.choice(["puoi", "lo puoi fare", "insomma"]) + " " + text
            if rng.random() < 0.25:  # two chatters
                pre += " " + rng.choice(JOIN) + " " + rng.choice(chat)
            s = " ".join(x for x in (pre, rng.choice(JOIN), text, rng.choice(END)) if x)
            row = (s, intent, args)
        if row[0] in seen:
            continue
        seen.add(row[0])
        out.append("\t".join(x for x in row if x))
    path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, "lunghe.tsv")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("%s: %d frasi, %d chiacchiere, %d comandi" % (path, len(out) - 2, len(chat), len(cmds)))


if __name__ == "__main__":
    main()
