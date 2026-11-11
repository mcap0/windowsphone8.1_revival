# Batteria del Lumia 1020: misurare, poi ridurre i consumi

Obiettivo: il consumo più basso possibile usando il root, con ogni modifica misurata prima e dopo. La batteria è vecchia
(BV-5XW, 2000 mAh nominali). Cambiarla resta un'opzione, ma il software deve consumare il minimo comunque.

## Strumenti

- `sonda/`: servizio nativo nello slot di sistema `InBoxPowerWDSvc` (ex DumbSvc), parte a ogni accensione. Ogni minuto
  registra batteria (%, carica, capacità piena, corrente; negativa = scarica), fonte (batteria/rete/carica), CPU totale e
  i 5 processi che hanno usato più CPU. Non usa la rete per misurare. Scrive `C:\Data\Users\Public\Documents\batteria.log`
  (4 MB, poi `batteria.old.log`) e ogni 5 minuti manda la coda al PC (`share/inbox/batteria.txt`) se l'inbox risponde.
  Fuori casa i dati restano in coda e arrivano al rientro.
- `report`: riassunto per periodi (`batteria/report [MM-DD HH:MM [MM-DD HH:MM]]`). Per ogni periodo a batteria dà
  assorbimento medio, durata stimata, CPU e processi; dopo 20 minuti confronta i punti percentuali persi con quelli spiegati
  dalla corrente misurata (un divario grande = batteria usurata o indicatore sbagliato). Poi dice quanta carica va nel
  riposo e quanta nei risvegli, e i 5 risvegli più cari con i processi in cima.

### Build e installazione

```
batteria/sonda/build.sh                       # VM → share/sonda.exe (non firmato: il telefono rootato lo esegue)
tools/esplora 'put sonda.exe C:\Windows\System32\sonda.exe'
tools/esplora 'svc stop InBoxPowerWDSvc'      # poi 5 s
tools/esplora 'rm C:\Windows\System32\InBoxPowerWDSvc.exe' \
              'cp C:\Windows\System32\sonda.exe C:\Windows\System32\InBoxPowerWDSvc.exe' 'svc start InBoxPowerWDSvc'
```

Il `cp` di Esplora non sovrascrive: serve il `rm` prima. `run` di Esplora non avvia eseguibili nativi (accesso negato):
la sonda si prova solo come servizio. Copia del vecchio DumbSvc: `C:\Windows\System32\sonda-old.exe`; originale
Microsoft: `backup/InBoxPowerWDSvc.exe.orig`.

## Unità: mA e mAh, confermate (notte 11-01/11-02)

Il driver dichiara progetto 2000, piena 1900, chimica LION. Nella notte 612 unità di corrente hanno portato l'indicatore
da 100% a 68%: 32 punti di 1900 fanno 608 unità. Se fossero mWh, 612 sarebbero 8 punti su 2000 mAh e non tornano.
Quindi corrente in mA e capacità in mAh, come già etichettato. La percentuale di Windows è calcolata dallo stesso
contatore, perciò "punti scesi" e "punti spiegati dalla corrente" ora coincidono (32 vs 31): l'indicatore non mente e
la batteria, a questo regime, non crolla. "Piena = progetto" significa che il driver non stima l'usura: l'usura si legge
dal divario nel `report`. Cicli: il driver riporta 0, non li conta. La temperatura invece la espone (vedi 1.1).

## Cosa sappiamo (2026-10-31 / 11-01, dai log delle app in background)

- Notte a casa in Wi-Fi, fermo: circa 2,3%/h, troppo per un telefono che dorme. Allora girava il vecchio DumbSvc:
  connessione TCP e toast ogni minuto (circa 1.440 risvegli della radio al giorno), ora sostituito dalla sonda.
- 31/10 dalle 11 alle 14:35, fuori su rete cellulare: circa 10%/h.
- 31/10 sera: carica al 100% alle 21:35, fuori casa; 20% alle 22:00, spento alle 22:05, caldo. 80% in 25 minuti
  vorrebbe dire circa 4 A: più di quanto il 1020 possa assorbire. Ipotesi principale: batteria che sotto carico (freddo,
  radio al massimo) crolla di tensione, mentre l'indicatore era tarato male. Il caldo dice che però qualcosa assorbiva.
  Il crollo non si è ripetuto l'11-02; resta da spiegare.

## Cosa sappiamo (notte 11-01 22:52 → 11-02 11:04, sonda 1.1)

612 mAh in 12,2 h, media 50 mA, 32 punti (2,6%/h), 39,8 h a questo ritmo. Nessun buco: 732 campioni su 732 minuti.
Il telefono dorme davvero: 54% dei minuti a 11-12 mA (minimo 9). Tutto il resto è veglia — il 29% dei minuti sta sopra
50 mA e brucia il 74% della carica. In ordine di costo:

- **Un risveglio ogni 30 minuti esatti** (:19 e :49, `BACKGROUNDTASKHOST.EXE`, cioè l'agente periodico di un'app):
  3-10 mAh a testa, ~24 volte a notte, 5-7 punti. È il costo ricorrente più grosso e il primo da togliere.
- **06:47, 5 minuti a 836 mA** (System, TaskHost, SVCHOST, OEMServiceHost): 40 mAh, 2 punti. La manutenzione del
  mattino; tensione scesa a 3791 mV sotto carico, senza crolli.
- Una coda di risvegli brevi ogni ~10 minuti da 1-3 mAh (System, SVCHOST, MobileUI, LSASS).
- **22:52-23:00, appena staccato il caricatore**: 74 mAh in 12 minuti con SVCHOST al 100% di un core. I primi 3-4 punti
  spariscono lì, non durante la notte: è il "sync" che parte quando togli la spina.

## Piano delle prove (una variabile alla volta, almeno 30 minuti a schermo spento)

1. **Base**: schermo spento in Wi-Fi a casa; schermo spento su rete cellulare (Wi-Fi spento); schermo acceso fermo.
2. **Rete cellulare**: stessa prova in 4G, in solo 3G e in solo 2G, nello stesso posto. Non si decide a priori: il 4G
   trasferisce i dati in un picco breve e torna a riposo prima del 3G. Il peso vero è il segnale debole e il passaggio
   4G↔3G, da provare anche in un posto con poco campo.
3. **Processi svegli a schermo spento**: dalla colonna processi del `report`. Servizi di sistema inutili per noi
   (telemetria, sync, Store/Xbox, aggiornamenti, Cortana/Bing) da disattivare dal registro uno alla volta, con copia del
   valore originale nel log di Esplora.
4. **Le nostre app in background** (Telegram, Posta, tailscale, Perle): costo di ogni risveglio e frequenza
   che serve davvero.
5. **Radio accessorie**: Bluetooth, NFC, posizione, Wi-Fi acceso senza rete nota (scansioni).
6. **Schermo**: luminosità, Glance (schermo sempre acceso), timeout.
7. **Più a fondo, col root**: limiti della CPU (frequenza massima, core) e profili di risparmio del power manager, solo
   dopo aver visto che a schermo spento il telefono dorme davvero.

Ogni risultato va annotato qui sotto: prova, condizioni, mA medi, durata stimata.

## Risultati

| Data | Prova | Condizioni | mA medi | Durata stimata | Note |
|---|---|---|---|---|---|
| 11-01 19:19-20:21 | base: casa Wi-Fi 17 min, poi fuori su rete cellulare (4G) 30 min, schermo spento | caricatore staccato dalle 19:19 | 137 → 277 (non affidabile) | — | CPU 1% costante, nessun processo sveglio. Percentuale ferma al 100% per un'ora e corrente che cambia solo ai risvegli (19:36, 19:45, 20:16): `NtPowerInformation` restituisce uno stato in cache, non letture fresche. Serve la lettura diretta dal driver (IOCTL batteria). |
| 11-01 22:52-11-02 11:04 | base notte: casa, Wi-Fi, schermo spento, nessuna app aperta | staccato alle 22:51; sonda 1.1 (driver) | 49 dal contatore (ora tipica 41, fondo 10) | 46 h all'ora tipica | Rifatto il 11-02 col report corretto: consumo dal contatore di carica, non dalla media degli istantanei; primi 58 min fermi al 100% esclusi; due ricalibrazioni dell'indicatore (00:10 -46 mAh, 11:02 -16 mAh) escluse. Ore tranquille 34-43 mAh/h; 06:47 manutenzione ~57 mAh (System, svchost, TaskHost); risvegli 49% del consumo (non 74%). **Resistenza interna ~285 mΩ** (10 salti, 103-370; sana ~150): a 2 A da 3,70 V si scende a ~3,1 V, sotto lo spegnimento. Spiega la sera del 31/10: batteria usurata, cede sotto carico e al freddo. |

## Prossimo passo

La sonda 1.1 fa quello che serviva. Il consumo ora è quasi tutto nei risvegli, non nel fondo. In ordine:

1. **Togliere l'agente dei 30 minuti** (`BACKGROUNDTASKHOST.EXE`): è il costo ricorrente più grosso. Guardare i task
   periodici delle app (Telegram, ponte, Posta, i nostri watch) e spegnerli uno alla volta, una notte
   ciascuno, confrontando `batteria/report`.
2. **Manutenzione delle 06:47** (System, TaskHost, SVCHOST, OEMServiceHost): 2 punti in 5 minuti. Cercare l'attività
   pianificata e, se non serve, disattivarla.
3. **Il pavimento**: una notte in modalità aereo con tutti gli agenti spenti. Se resta ~10 mA, il minimo del telefono è
   1900/10 = 190 h; poi si aggiunge una cosa alla volta (Wi-Fi, poi un'app).
4. **La sonda deve registrare anche la rete** (Wi-Fi/cellulare/aereo, e il contatore di traffico): senza, un risveglio
   non si può attribuire alla radio o a un'app.
5. Solo dopo: CPU e frequenze (§7 del piano delle prove).

