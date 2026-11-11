/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The Italian strings of res/values-it/strings_cast.xml.
 */

namespace Perle.Cast
{
    public static class CastStrings
    {
        public const string CastToTv = "Trasmetti a TV";
        public const string Searching = "Ricerca in corso…";
        public const string Retry = "Riprova";
        public const string NoDevices = "Nessun TV trovato. Controlla che telefono e TV siano sulla stessa rete Wi-Fi e che il TV sia acceso, poi tocca Riprova.";
        public const string PlayingOn = "Su: {0}";
        public const string NotificationText = "{0} · {1}";
        public const string PlayPause = "Play/Pausa";
        public const string Stop = "Stop";
        public const string VolumeUp = "Alza volume";
        public const string VolumeDown = "Abbassa volume";
        public const string Volume = "Volume {0}";
        public const string CopyLog = "Copia log";
        public const string ClearLog = "Cancella";

        public const string StatusConnecting = "Connessione…";
        public const string StatusBuffering = "Caricamento sul TV…";
        public const string StatusPlaying = "In riproduzione";
        public const string StatusPaused = "In pausa";
        public const string StatusStopped = "Fermato";
        public const string StatusError = "Errore";

        public const string ErrorNoWifi = "Il telefono non è connesso a una rete Wi-Fi";
        public const string ErrorNoStream = "Questo video non ha uno stream trasmissibile (serve un MP4 con audio fino a 720p, oppure stream video H.264 e audio AAC)";
        public const string ErrorTv = "Il TV ha rifiutato la richiesta: {0}";
        public const string ErrorLiveRefused = "Il TV ha rifiutato lo stream MPEG-TS in diretta: {0}";
        public const string ErrorUnreachable = "Il TV non è raggiungibile";

        public const string DirectLinkTitle = "Invia link diretto";
        public const string DirectLinkSummary = "Passa al TV il link originale del video invece di farlo passare dal telefono. Solo per TV che supportano HTTPS; YouTube potrebbe rifiutare il TV.";
        public const string LogTitle = "Log Trasmetti a TV";
        public const string LogSummary = "Registro tecnico degli ultimi tentativi, da allegare a una segnalazione";
        public const string LogEmpty = "Il log è vuoto";

        public const string PlayNow = "Riproduci ora sul TV";
        public const string AddToQueue = "Aggiungi alla coda del TV";
        public const string AddedToQueue = "Aggiunto alla coda del TV";
        public const string OpenController = "Apri i controlli del TV";
        public const string OtherTv = "Trasmetti a un altro TV";
        public const string Next = "Successivo";
        public const string Queue = "Coda";
        public const string Quality = "Qualità";
        public const string QualityAuto = "Automatica";
        public const string QualityAutoCurrent = "Automatica ({0})";
        public const string Subtitles = "Sottotitoli";
        public const string SubtitlesOff = "Sottotitoli disattivati";
        public const string SubtitlesHint = "Se i sottotitoli non compaiono, attivali nelle impostazioni dei sottotitoli del TV";
        public const string QualityFallback = "Il TV non riusciva a riprodurre {0}, ora in {1}";
        public const string ErrorCannotPlay = "Il TV non riesce a riprodurre questo video in nessuna qualità";
        public const string LastUsed = "{0} · usato l'ultima volta";
        public const string ErrorTvBusy = "Il TV è occupato o spento: accendilo, chiudi quello che sta mostrando e riprova";
        public const string SentToTv = "In riproduzione su {0}";
        public const string AudioTrack = "Traccia audio";
    }
}
