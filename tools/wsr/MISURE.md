# wsr — registro delle misure (passaggi in avanti)

Regola (VOCE-PIANO.md §2 e §5): ogni misura si annota con set e data. Un set usato per correggere diventa "visto"
(sviluppo), e la misura successiva si fa su un set nuovo.

## Set

| Set | Cosa | Stato |
|---|---|---|
| `pa-pi-pu-it3` | "pa pi pu" detto da mb-it3, s110 | **visto** (2026-11-03): usato per sistemare `ear.py` (ordine LPC 12, soffitto 5c/2L, F0 sul bordo dell'intervallo) |
| `sillabe1` | `vowels.py --test`: 56 sillabe accentate (8 consonanti × à è é ì ò ó ù) + 21 parole in 3 elenchi; voci mb-it1/2/3/4, velocità 120 e 175, scala 1.0 e 1.15 (tratto più corto del 15%) | primo uso: vedi sotto |

## Misure

### 2026-11-03 — `sillabe1`, primo uso (mai visto prima dal rilevatore delle vocali)
9200 finestre centrali di vocali accentate. **7 vocali 45.1%, 5 vocali 57.4%.** Senza formanti: 2853 (31%).
Per voce (7/5): it1 ♂ 30–44 / 30–57; it2 ♀ 26–36 / 49–65; it3 ♂ 68–75 / 76–83; it4 ♀ 22–38 / 33–55.
Per vocale (5): i 62, e 61, a 52, o 50–73, u 25 (847 finestre /u/ su 1372 senza formanti).
Da qui `sillabe1` è **visto**: le correzioni si fanno su questo set, la prossima misura su `sillabe2` (nuovo).

### 2026-11-03 — correzioni su `sillabe1` (sviluppo), 3 giri
1. F3/F4 facoltativi nel tracciatore, L dalla dispersione dei formanti (Fitch 1997): 5 vocali 57.4 → 59.0%.
2. Trovati due difetti del *set*, non del riconoscitore: espeak manda a it1/it2 nomi di fonemi che non esistono
   (`P-U1 unknown` → silenzio etichettato come vocale); il database it2 ha una `U` con F2 a 2410 Hz (non è /u/).
   Ora tutte le voci ricevono la trascrizione di it3, tradotta nei nomi di it1 misurati uno per uno; it2 tolto.
   Sonorità di Boersma (1993) al posto del centre clipping; VTLN a massima verosimiglianza (Lee & Rose 1996) con
   prior antropometrico N(16, 1.5 cm): 70.2%.
3. Soffitto LPC con L limitata a 13–18.5 cm (una L sbagliata fondeva F1 e F2 della /u/), attrazione al tubo neutro
   quasi nulla, penalità per le risonanze strette saltate sotto F2: **7 vocali 62.9%, 5 vocali 75.9%**.

### 2026-11-03 — `sillabe2`, primo uso (mai visto)
Consonanti b d g n v r ci gi, 21 parole nuove, voci it1 ♂ it3 ♂ it4 ♀, velocità 140 e 200, scala 0.95 / 1.08 / 1.25.
7259 finestre. **7 vocali 55.5%, 5 vocali 69.6%.** (sviluppo: 62.9 / 75.9). 126 frasi rifiutate da mbrola (ci/gi
e d su it1, un elenco di parole).
Per voce, 5 vocali: it3 75–82%; it1 48–68%; it4 70–76% a scala ≤1.08, **44–48% a scala 1.25** (taglia bambino:
la L stimata resta ~14.5 invece di ~12.7, il prior per adulti la trattiene).
Per vocale (5): i 72, e 70, a 55, o 74, u 49. Confusioni principali: u→o (29%), a→ɔ (18%), a senza formanti (22%).
Da qui `sillabe2` è **visto**.

### 2026-11-03 — `speaker` (registrazioni vere, Lumia 1020, microfono Enhanced, Assistente 1.3.6), primo uso
`tools/wsr/realtest.py`: 7 vocali tenute (solo il primo tratto sonoro: la frase dello schermo è stata letta tutta,
"è aperta, come in bène") + 12 serie di sillabe (10 usabili, 5 vocali ciascuna). **5 vocali 64.8%, 7 vocali 50.5%.**
Sillabe: a **1.7%** (sempre /ɔ/), e 94, i 70, o 70, u 89. Causa: il tracciatore salta F3 vero (2750 Hz, banda 185)
per F4 (3850): L stimata 12–15 cm su una voce maschile, i formanti scalati in basso, /a/ 700/1150 → /ɔ/.
Da qui `speaker` è **visto**.
Dopo la penalità sulle risonanze saltate fino all'ultimo formante: 5 vocali 67.8% (a resta 2.9%: F2 della /a/
di speaker ≈ 1150 Hz, tra la /a/ 1300 e la /ɔ/ 980 della tabella). Analisi per sintesi (v2, `WSR_VOWELS=abs`):
prima versione 37.5%, peggio; resta spenta finché non supera v1.

### 2026-11-03 — lettura `testospeaker` (89 frasi, Lumia), fit permesso dall'utente
Etichette: `dtwalign.py` (DTW sugli MFCC contro MBROLA it3 che dice la stessa frase, alla velocità della
registrazione). Controllo su MBROLA, centri delle vocali dentro la vocale vera: it4 ♀ 91%, it1 ♂ 100%, it4 veloce 73%
(l'allineatore a regole `align.py` faceva 59%). Fit sulle frasi dispari, misura sulle pari (mai usate):
| rilevatore | 5 vocali | 7 vocali |
|---|---|---|
| formanti, tabella Ferrero | 42.5% | 29.0% |
| formanti, tabella fittata (`fit.py`, `lumia.json`) | 36.4% | 27.2% |
| **gaussiana per vocale sugli MFCC normalizzati sulla frase** (`mfccfit.py`) | **64.1%** | 49.3% |
Nel parlato letto i formanti sono il collo di bottiglia (25% delle finestre senza formanti, /u/ con 2.6 Bark di
dispersione): adattare la tabella non aiuta. Gli MFCC non hanno niente da tracciare. Prossima misura vera: `testomamma`.

### 2026-11-03 — modello MFCC su speaker: correzioni a mano (frasi pari, mai fittate; 5 vocali)
- `dtwalign`: silenzio iniziale e finale tagliati prima della DTW (la DTW spalmava i primi fonemi sul silenzio di
  ~0.7 s): per finestra 64.3 → **68.6%**. (Controllo MBROLA ora su più frasi: it4 82%, it1 100%.)
- media degli MFCC su 5 finestre (±20 ms): 70.7% per finestra, 70.7% per segmento.
Budget di passaggi in avanti sulle frasi pari, dopo la lettura dei pesi (`edit.py`):
1. A (delta tolti: media ≈ 0 per tutte le vocali, separazione 0.1–0.3 contro 0.4–1.1 dei cepstri; una larghezza per
   tutte le vocali): neutra, tenuta. Segmenti 70.7%.
2. C (prior delle vocali dalla frequenza delle lettere italiane, u 3%, e/o chiuse 4× le aperte): **72.2%**, u 76.
3. D (decisione a 5 sommando e+ɛ, o+ɔ): 72.2%, invariata, tenuta.
4. (solo diagnosi) accentate 77.2%, atone 67.8%; segmenti ≥ 120 ms 85.7%.
Podcast: la registrazione arriva a p90 −48 dB (25 dB sotto la lettura): la correlazione con l'originale è piatta
(0.17), non si sincronizza. Da rifare col volume più alto e il telefono vicino alla cassa, minuti 0–13:40.

### 2026-11-03 — pesi a mano in `tools/wsr/vocali.txt` (frasi pari di speaker, 5 vocali per segmento)
Il modello è ora un file di testo: 7 centri × 12 cepstri, 12 larghezze, 7 prior; ogni modifica è scritta sotto la riga.
- base (= modello A+C): 72.2%
- H: centro /u/ 15% più lontano da /o/ (o→u era l'errore più frequente): **73.3%** (o 69 → 73). Tenuta.
- I: centro /a/ 10% più lontano da /ɔ/: 72.2% (a 72 → 69). Tolta.
Decodificatore (`decode.py`, 552 frasi candidate, 28 comandi di speaker): DTW sola 9/28 primi, 14/28 nei primi 5;
più evidenza delle vocali (BETA 0.15) **11/28**; BETA 0.4 10/28; delta nella DTW 7/28 (tolti).
Podcast `20261103-160828`: sincronizzato (primo pezzo a 4:11, correlazione 1.05), 27 frasi; il modello di speaker
su Sanfilippo: 34% (altra voce, attraverso le casse: da affrontare dopo).
- J: centro /e/ 10% più lontano da /a/: 73.0%. Tolta.
- K: larghezza di c1 22.2 → 17.8: 73.3%, invariata. Tolta.
Gli spostamenti a mano dei centri sono arrivati a ~73%: gli errori rimasti sono soprattutto vocali atone brevi
(e probabilmente etichette sbagliate); servono più dati (`registra comandi`) e l'evidenza della frase intera.

### 2026-11-03 — solo la voce di speaker (scelta dell'utente)
`fit.dataset`: fit = 182 registrazioni (lettura, comandi dispari, prima sessione 8–50); prova = 50 comandi pari.
- Modello vocali vecchio (lettura sola + H) sui 50 comandi di prova: 72.8% per segmento; fit su tutti i 182: 71.1%.
  Tenuto il vecchio. Il set di prova è stato guardato per questo confronto (2 passaggi): ora resta per il decodificatore.
- Sviluppo = comandi dispari: modello fittato 70.4%; centri dalle curve lette a occhio (`spettri-speaker.txt`,
  `spettri.py`, solo formule) 68.0% (e 78, u 67 meglio; a, o peggio); misto (e, u a occhio) 69.2%. Tenuto il fittato.
- `speakermap.py`: mappa affine MBROLA → speaker da 43857 coppie di finestre: distanza media 3.62 → 2.91.
Giro di prova (`prova.py nuovo` → "registra prova" → `prova.py esito`): ogni giro 40 frasi nuove.

### 2026-11-03 — `wsr` sul telefono (Assistente 1.4.0), C++ `Core/Wsr.cpp`
Parità C++/Python sugli MFCC: energia entro 5e-5 dB, cepstri entro 1e-4. `voce.wsr` 2.3 MB, 592 frasi.
Decodificatore del telefono (DTW su tutte + evidenza delle vocali sulle 20 migliori, col percorso del modello):
- sviluppo (78 comandi): frase giusta 28/78; azione giusta (stesso intento, rete wsp) 39/78.
- **prova (50 comandi pari, primo uso per il decodificatore): frase giusta 16/50, azione giusta 29/50 (58%),
  azione sbagliata 21/50.** Il distacco non separa bene: a distacco ≥ 0.15, 20 giuste e 10 sbagliate.
- 118 ms a frase sul PC. Nell'app la frase si mostra e parte solo con la freccia (niente azioni da sola).
- La mappa MBROLA → speaker sulle sole DTW peggiora (8 → 7 su 28): non usata.

### 2026-11-03 — Vosk (Kaldi, modello vosk-model-small-it-0.22, 48 MB zip / 88 MB) sugli stessi 50 comandi di prova
| | frase esatta | azione giusta | WER | ms/frase (PC) |
|---|---|---|---|---|
| wsr (nostro, 1.4.0) | 16/50 | 29/50 | — | 118 |
| Vosk libero | 34/50 | 48/50 | 11.0% | 369 |
| Vosk con grammatica (618 frasi, senza cifre) | 46/50 | 49/50 | 2.4% | 68 |
Decisione: porting di Vosk su WP8.1 (PLAN.md §59.3); wsr resta come esperimento e come ripiego.
