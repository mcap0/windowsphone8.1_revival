# Riconoscitore vocale italiano scritto a mano (senza addestramento) — piano

Data: 2026-11-03. Da leggere per primo dopo un `/clear`. Riassume a che punto siamo, cosa dice l'articolo che
seguiamo, e come costruire il riconoscitore vocale di Assistente con lo stesso metodo della rete degli intenti.

---

## 1. Perché, e da dove partiamo

**Obiettivo dell'utente:** Assistente (app WP8.1 in C++/CX, `apps/Assistente`) deve funzionare offline per quasi
tutto, e usare Gemini solo quando serve. La parte testo → comando esiste già ed è scritta a mano. Manca la parte
**voce → testo**: il riconoscitore di Windows non va su questo telefono, e l'utente vuole un riconoscitore nostro,
**scritto a mano, senza addestramento, avanzato e funzionante per tutti** (indipendente dal parlante).

**Cosa è stato provato con il riconoscitore Microsoft** (PLAN.md §59.1). Tutte e tre le strade sono finite:
1. Dati vocali it-IT 14226 sul motore 12393 (firmware turkey 8.10.12393, che l'utente vuole tenere):
   `RecognizeAsync` falliva in ~40 ms con `0x800455A0`.
2. Dati it-IT della 12393 presi dal WPAK su archive.org (`wp8.1-partner-drop`, in `upgrade/wpblue-12393/`) e messi
   in `C:\Data\SharedData\Speech\it-it`: il motore parte, ma risponde sempre "sentito "", rifiutato" dopo gli 8 s
   di attesa del silenzio. Usa il microfono predefinito `{669e1dc8…}`, che sul Lumia 1020 dà silenzio; quello che
   funziona è l'"Enhanced Audio Recording Device" `{eebe3b05-eaf0-4a7c-b50e-7565b5483ab3}`.
3. Role:0/1/2 (data 2030) scritti sull'Enhanced nel registro MMDevices + riavvio: nessun effetto.
   **Da annullare** alla prossima apertura di Esplora (console accesa):
   `tools/esplora "regdel HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{eebe3b05-eaf0-4a7c-b50e-7565b5483ab3} Role:0"`
   (poi lo stesso per `Role:1` e `Role:2`) e riavvio.

**Cosa funziona già sul telefono:**
- La nostra cattura WASAPI (`apps/Assistente/Assistente/Capture.cpp`), dal microfono Enhanced, cuffie o Bluetooth:
  PCM 16 kHz mono, con fine frase automatica (`Core/Audio.cpp`, classe `Vad`).
- La rete scritta a mano testo → intento (§3), in C++ (`Core/Wsp.cpp`, `Core/Command.cpp`): ~10 µs a frase sul PC,
  0.4–0.8 ms sul telefono. Dal testo scritto nella casella esegue già i comandi offline.
- Assistente 1.3.5 installata (`UseGemini = false` in `MainPage.xaml.cpp`: tutto ciò che non capisce mostra
  "Non ho capito" con la diagnosi). I giri 6–7 della rete (commit `03fcbd3`) non sono ancora sul telefono.

---

## 2. L'articolo che seguiamo

**"Sparks of Weight-Space Programming: LLMs can natively read & write neural network weights"**, Rikiya Takehi,
Jayden Teoh, Alex Zhang, Omar Khattab (MIT CSAIL), 2 novembre 2026.
<https://rikiyat.github.io/blog/weight-space-programming/>. Log completi: <https://github.com/RikiyaT/lm_understand-logs>.

L'idea: un LLM generico legge e scrive **direttamente i pesi** di una rete a partire da un obiettivo descritto a
parole. Al posto dell'ottimizzazione c'è il ragionamento. Frasi del testo che contano per noi:

- Esperimento 5 (Breakout, Seaquest, Asterix da zero, MLP 600→32→32→6 con tutti i pesi a 0):
  *"Training: none: no data, no loss, no gradients, and no tools. Astra writes the weights as text, in one reply,
  and the network never plays before it is scored."* — *"Astra reasons about the desired programs it wants each
  network to implement and converts them into neural form."* Breakout 517.4 contro 15.8 di PPO a 100M passi.
- Esperimento 6 (Go 9×9, MLP 243→512×6→82, 1.48M pesi): *"Astra writes the network as a small Go program, layer by
  layer, in Python."* — *"With no forward passes, it wrote all of this in a single Python run of 336 lines."*
  Strumenti: una sandbox Python per leggere e salvare i tensori, senza poter eseguire la rete. **Regole:**
  *"Not allowed: training or fitting the weights to data, search (no tree search, no rollouts), and running the
  network outside the forward passes."* Con 50 passaggi in avanti batte il PPO della stessa taglia addestrato su
  20 milioni di partite.
- Esperimento 8 (Qwen3.5-0.8B): modifiche mirate in un kernel Python, *"with no training"*. MATH-500 da 7.9 a 48.1.
- Il limite dichiarato (Gemma 3 270M): le modifiche *"could remove an obstacle that hid an ability, but it could not
  add an ability the model lacked."*
- I compilatori di programmi in pesi sono citati come metodo già esistente; la novità rivendicata è il modello che
  ragiona nello spazio dei pesi.

**Come lo applichiamo (le nostre regole, prese dall'esperimento 6):**
1. Ogni peso è scritto da codice di costruzione leggibile, e ogni unità ha un commento su cosa calcola.
2. **Vietato fissare i pesi sui dati:** niente gradienti e niente medie o statistiche calcolate sulle registrazioni
   di prova per decidere i pesi. Le costanti vengono da conoscenza scritta: fonetica acustica, ortografia
   italiana, letteratura.
3. I dati servono solo come **passaggi in avanti** per misurare, come nell'esperimento 6. Teniamo il conto delle
   misure fatte, come il loro budget di 0/10/25/50.
4. Prima di ogni correzione si misura su un set **mai visto**, e il numero si annota. Il set su cui si corregge
   diventa set di sviluppo, e il giro dopo si scrive un set nuovo (come in `parafrasi*.tsv`).

---

## 3. Cosa esiste già: la rete degli intenti scritta a mano (`tools/wsp/`)

- `assistente.py`: costruisce i pesi e li scrive in `apps/Assistente/Assistente/Assets/intent.wsp` (2.4 MB, ~57k
  pesi, quasi tutti a 0). La struttura:
  - strato −1: trigrammi di lettere (1024 secchi FNV-1a) → parola nota più vicina, per errori di battitura e forme
    flesse, con la stessa prima lettera;
  - embedding: 59 classi di parole scritte a mano;
  - convoluzione −3..+1 (70 unità: "numero prima di minuti", "numero dopo alle", "si + chiama", ecc.);
  - tag per parola (qty, hour, minute, span, unexplained), con una testa a media causale ("c'è stato un verbo che
    apre un nome?");
  - 50 regole AND/NOT;
  - 18 intenti.

  Tutte le unità sono `clip01`, cioè porte logiche.
- `grammar.py`: grammatica SRGS dei comandi, più un campionatore di frasi. Per la voce diventa **il vincolo del
  decodificatore** (§4.4).
- Misure: `golden.tsv` (113), `parafrasi.tsv`…`parafrasi7.tsv`, `colloquiale.tsv`. Strumento: `eval.cpp`
  (compilarlo come scritto nella sua prima riga). Tutti i set sono al 96–100%, con 0 azioni sbagliate; ogni stile
  nuovo parte dal 62–79% su frasi mai viste.
- Runtime: `Core/Wsp.cpp` (stesso tokenizer e forward di Python, con NEON), `Core/Command.cpp` (argomenti; la
  funzione `Understand` passa la rete su ogni pezzo della frase).
- Dataset per la misura (non per costruire): MASSIVE (Amazon, 2022), ~16.5k frasi italiane da assistente vocale.
  È il prossimo banco di prova per la parte testo.

---

## 4. Il riconoscitore vocale scritto a mano

Nome di lavoro: **`wsr`** (weight-space recognizer). File: `tools/wsr/` (costruzione e misure in Python),
`apps/Assistente/Assistente/Core/Wsr.{h,cpp}` (runtime C++), pesi in `Assets/voce.wsr`.

**Idea guida:** non riconosciamo l'italiano libero. **Scegliamo tra le frasi della nostra grammatica**, cioè
qualche migliaio di forme più i nomi dei contatti e degli artisti. È un problema molto più facile, e ogni pezzo si
può scrivere da conoscenza nota.

### 4.1 Orecchio: strati a formula fissa (pesi scritti per definizione)

Audio PCM 16 kHz dalla nostra cattura → finestre di 25 ms ogni 10 ms:
- pre-enfasi `x[n] − 0.97 x[n−1]` (un filtro a 2 pesi);
- finestra di Hamming, poi **DFT a 512 punti** come matrice di coseni e seni (i pesi sono `cos(2πkn/N)`);
- **banco di 40 filtri mel** (matrice di triangoli da 0 a 8000 Hz), logaritmo, **DCT** → 13 MFCC con le derivate;
- **energia**, **periodicità** (autocorrelazione normalizzata: è voce?) e **F0**;
- **formanti F1, F2, F3**: picchi dello spettro liscio (predizione lineare con Levinson-Durbin, una formula, non un
  addestramento) con continuità tra finestre.

**Normalizzazione del parlante, scritta a mano** (la chiave del "per tutti"). La lunghezza del tratto vocale (VTL)
scala i formanti: un tubo chiuso da un lato ha risonanze a `(2k−1)·c / 4L`, con c ≈ 35.000 cm/s, e L ≈ 17.5 cm per
un uomo adulto, ~14.5 per una donna, ~12 per un bambino. Stimiamo L dalla media di F3, e di F4 quando c'è, sui
tratti vocalici della frase stessa (quindi senza dati di altri), e scaliamo tutte le frequenze verso il parlante di
riferimento: `f' = f · L / 17.5`. È la VTLN classica, con una formula fisica al posto di una stima statistica.

### 4.2 Fonetica: rilevatori scritti (uno strato per classe di suoni)

Ogni finestra dà un punteggio per ciascun **fonema italiano**, scritto dalla fonetica acustica:
- **Vocali (7):** /i e ɛ a ɔ o u/ come regioni nel piano F1–F2 normalizzato. Valori di partenza dalla letteratura
  sulle vocali italiane (Ferrero 1972; Albano Leoni & Maturi, *Manuale di fonetica*), per esempio /a/ F1≈750
  F2≈1300, /i/ F1≈300 F2≈2300, /u/ F1≈320 F2≈750 Hz, scalati per la VTL. Il punteggio è una gaussiana scritta a
  mano su (F1, F2), con larghezze dalla stessa letteratura.
- **Nasali /m n ɲ/:** voce con energia concentrata sotto i 500 Hz, spettro alto debole ("murmur"), F1 ≈ 250 Hz.
  Luogo dalla transizione di F2 verso la vocale vicina (m: F2 scende; n: ~1700; ɲ: alto).
- **Laterali /l ʎ/, vibrante /r/:** l: voce con F3 alto e continuità; r: una o due brevi interruzioni (1–3 finestre)
  in un tratto di voce.
- **Fricative /f v s z ʃ/:** rumore senza periodicità (v e z con voce), distinte dal **baricentro spettrale**:
  s > 4500 Hz, ʃ ≈ 2500–3500, f diffusa e debole.
- **Occlusive /p b t d k g/:** silenzio (chiusura) poi scoppio. Sorde o sonore dalla barra di voce durante la
  chiusura; luogo dal baricentro dello scoppio (p basso, t alto, k medio) e dalle transizioni di F2.
- **Affricate /ts dz tʃ dʒ/:** occlusiva poi fricativa, con il baricentro dell'una o dell'altra.
- **Silenzio e rumore:** energia vicina al rumore di fondo, stimato dai primi 200 ms della cattura (la nostra
  `Vad` lo fa già).

Ogni rilevatore è una piccola funzione o strato con costanti commentate. Uscita: una matrice finestre × 30 fonemi
di log-punteggi.

### 4.3 Pronuncia: regole dell'ortografia italiana (G2P scritto a mano)

L'italiano si legge quasi come si scrive. Regole: c/g davanti a e/i (dolci) o a/o/u (dure); ch/gh; gli → /ʎʎ/;
gn → /ɲɲ/; sc+e/i → /ʃʃ/; s sonora tra vocali e davanti a consonante sonora (/zveʎʎa/); z → /ts/ o /dz/ (lista di
eccezioni); doppie = durata lunga; accento tonico dalla grafia, da un piccolo lessico e dalla penultima per
regola. Le parole straniere frequenti negli artisti e nei contatti (Pink Floyd, Queen, Coldplay) hanno un piccolo
lessico di eccezioni scritto a mano, e sono la prima fonte di errori attesa.

Si applica a tutta la grammatica (`grammar.py`), ai 741 contatti (dalla cache `contatti.txt`) e agli artisti
(quando ci saranno).

### 4.4 Decodifica: allineamento sulla grammatica

- Ogni fonema è un piccolo automa a 3 stati (inizio, centro, fine), con **durate minime e massime scritte**: vocali
  40–250 ms, occlusive 30–150 ms, doppie ×1.8, ecc.
- La grammatica (§3, `grammar.py`) diventa un grafo di parole, e ogni parola il suo grafo di fonemi.
- **Viterbi** sul grafo: costo = somma dei log-punteggi del §4.2 più le penalità di durata. Non è una ricerca
  vietata dall'articolo: è il calcolo esatto di un allineamento, come una moltiplicazione di matrici.
- **Confidenza:** si confronta il punteggio della frase migliore con un **modello "altro"**, cioè un ciclo libero
  di fonemi senza grammatica. Margine sopra soglia → testo alla rete degli intenti. Altrimenti "Non ho capito"
  (o Gemini, quando torna acceso).
- Costo sul telefono: le frasi possibili sono migliaia ma condividono i prefissi (albero). Stima: 300 finestre ×
  qualche migliaio di stati attivi con potatura a fascio → decine di ms per frase con NEON. Va misurato.

### 4.5 Dove si attacca in Assistente

`MainPage::StartCapture` → la nostra cattura WASAPI (che già funziona) → a fine frase il PCM va a `Wsr` →
testo + confidenza → `TryOffline` (che esiste già) → `Understand` → azione. `OfflineCommand` (il riconoscitore
Microsoft) si toglie quando `Wsr` è pronto.

---

## 5. Come misuriamo (passaggi in avanti, non costruzione)

| Set | Cosa | Uso |
|---|---|---|
| sintetico | frasi della grammatica dette da espeak-ng (voci it, it-mbrola 1/3/4), a velocità e altezze diverse | giri rapidi sul PC, controllo che la catena funzioni |
| utente | 50–100 comandi veri dal telefono (salvati come WAV in `share/inbox` o via `tools/phonelog`) | la misura che conta |
| altri parlanti | le stesse frasi da 2–3 persone diverse (uomo, donna, ragazzo) | "per tutti" |
| vocali | FLEURS italiano (pubblico): solo per controllare che i rilevatori di vocali scattino nei tratti giusti | controllo dei rilevatori |

Metriche: frase giusta (dopo la rete degli intenti: azione giusta), rifiuti corretti di frasi fuori grammatica,
**falsi comandi** (la metrica da tenere a 0, come per il testo), tempo sul telefono.

**Regola anti-imbroglio:** si annota ogni misura con il set e la data. Se un set viene usato per correggere, si
dichiara "visto" e la misura successiva si fa su registrazioni nuove.

---

## 6. Fasi

1. **Orecchio + vocali** (`tools/wsr/ear.py`, `vowels.py`): MFCC, F0, formanti, VTLN; rilevatore delle 7 vocali.
   *Prova:* sulle vocali isolate e sulle sillabe (sintetiche e registrate dall'utente), vocale giusta nella
   maggioranza delle finestre centrali, per voce maschile e femminile. **Se questa fase non va, il resto non ha
   senso:** fermarsi e decidere con l'utente.
2. **Tutti i fonemi** (`phones.py`): nasali, fricative, occlusive, liquide, affricate, silenzio.
   *Prova:* matrice di confusione per classi grandi (vocale / nasale / fricativa / occlusiva / silenzio) sui dati
   sintetici e su quelli dell'utente.
3. **G2P** (`g2p.py`): pronuncia di tutta la grammatica e dei contatti; test con una lista di parole scritta a
   mano con la trascrizione attesa.
4. **Decodificatore** (`decode.py`) sulla grammatica, con il modello "altro". *Prova:* comandi sintetici, poi
   dell'utente; frase giusta, falsi comandi, rifiuti.
5. **C++ sul telefono** (`Core/Wsr.cpp`, NEON), stessi risultati di Python sulle stesse WAV (come `parity` per la
   rete degli intenti); tempo per frase nel log (`wsr: … ms`).
6. **Altri parlanti** e correzioni a mano (scritte, mai fissate sui dati); poi contatti e artisti.

Ogni fase si chiude con un commit e una riga in PLAN.md §59.

---

## 7. Rischi (detti chiaramente)

- La storia: i riconoscitori a regole degli anni '70 (per esempio HEARSAY, Harpy) funzionavano solo con vocabolari
  piccoli e parlanti noti. Noi abbiamo vocabolario chiuso, grammatica, VTLN e un decodificatore esatto, ma è
  comunque la parte più incerta dell'intero progetto.
- Formanti: la stima dei formanti sbaglia con voci acute, rumore e il microfono del telefono. Il rilevatore di
  vocali deve dare punteggi morbidi, mai decisioni secche.
- Consonanti brevi (occlusive, r): sono le più difficili. Il decodificatore deve contare soprattutto sulle vocali
  e sulle durate, e usare le consonanti come indizi.
- Nomi stranieri: il G2P italiano li pronuncia male, quindi servirà il lessico di eccezioni.
- Se alla fase 1 o 4 i numeri sono bassi su voci vere, si torna dall'utente con i dati. Le alternative: modelli
  dalla sua voce (DTW, "opzione A"), oppure un modello addestrato.

---

## 8. Dopo il `/clear`: da dove ripartire

1. Leggere questo file e PLAN.md §59–59.1.
2. Annullare i tre valori Role nel registro (§1, punto 3), quando l'utente apre Esplora.
3. Fase 1: `tools/wsr/ear.py`, poi `vowels.py`. Strumenti sul PC: numpy, espeak-ng (voci `it`, `mb-it1/3/4`),
   ffmpeg. scipy non c'è: le formule sono scritte a mano comunque.
4. Chiedere all'utente le registrazioni di vocali e comandi dal telefono, e se ci sono altre persone disponibili.
