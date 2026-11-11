/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Shared playback state and MediaElement for the lifetime of the app shell.
 */

using Windows.UI.Xaml.Media;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Perle.Extractor;
using Perle.Progressive;
using Windows.Storage;
using Windows.UI.Core;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml;

namespace Perle81.App
{
    public sealed class PlayerService
    {
        static PlayerService current;

        readonly DispatcherTimer positionTimer;
        List<ProgressiveFormat> candidates = new List<ProgressiveFormat>();
        int candidateIndex = -1;
        int generation;
        bool resumed;
        TimeSpan? pendingSeek;
        HlsSource hls;
        string fallbackReason;
        string qualityText;
        string errorText;

        public static PlayerService Current { get { return current; } }
        public static PlayerService CurrentOrNull { get { return current; } }

        public static PlayerService Initialize()
        {
            if (current == null) current = new PlayerService();
            return current;
        }

        PlayerService()
        {
            Player = new MediaElement
            {
                AutoPlay = true,
                AreTransportControlsEnabled = false,
                Stretch = Windows.UI.Xaml.Media.Stretch.Uniform
            };
            Player.MediaOpened += OnMediaOpened;
            Player.MediaFailed += OnMediaFailed;
            Player.CurrentStateChanged += OnCurrentStateChanged;

            positionTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
            positionTimer.Tick += OnPositionTimerTick;
            positionTimer.Start();
        }

        public event EventHandler Changed;

        public MediaElement Player { get; private set; }
        public string CurrentUrl { get; private set; }
        public StreamInfo CurrentInfo { get; private set; }
        public bool HasVideo { get { return CurrentInfo != null && !string.IsNullOrEmpty(CurrentUrl); } }
        public bool IsPlaying { get { return Player.CurrentState == MediaElementState.Playing; } }
        public string QualityText { get { return qualityText ?? ""; } }
        public string ErrorText { get { return errorText ?? ""; } }
        public int AudioOptionCount { get { return hls == null ? 0 : hls.AudioOptions.Count; } }
        public IEnumerable<AudioOption> AudioOptions { get { return hls == null ? Enumerable.Empty<AudioOption>() : hls.AudioOptions; } }
        public AudioOption SelectedAudioOption { get { return hls == null ? null : hls.SelectedAudioOption; } }

        public async Task<StreamInfo> OpenAsync(string url)
        {
            if (string.IsNullOrEmpty(url)) return null;
            if (string.Equals(CurrentUrl, url, StringComparison.OrdinalIgnoreCase) && CurrentInfo != null)
                return CurrentInfo;

            Close();
            CurrentUrl = url;
            int requestGeneration = generation;
            fallbackReason = null;
            qualityText = "";
            errorText = "";
            candidates = new List<ProgressiveFormat>();
            candidateIndex = -1;
            resumed = false;
            pendingSeek = null;
            NotifyChanged();

            try
            {
                StreamInfo info = await YoutubeStream.GetInfoAsync(url);
                if (requestGeneration != generation) return null;

                CurrentInfo = info;
                await AppState.AddToHistoryAsync(info);
                if (requestGeneration != generation) return null;

                string preferredAudio = ApplicationData.Current.LocalSettings.Values["audioLang"] as string;
                if (!string.IsNullOrEmpty(info.HlsUrl))
                {
                    try
                    {
                        HlsSource openedHls = await HlsSource.OpenAsync(info.HlsUrl, 720, preferredAudio);
                        if (requestGeneration != generation) return null;
                        hls = openedHls;
                        hls.Failed += OnHlsFailed;
                        AudioOptionCountChanged();
                        qualityText = "Qualità: " + hls.Label + ".";
                        Player.SetMediaStreamSource(hls.Source);
                    }
                    catch (Exception ex)
                    {
                        if (requestGeneration != generation) return null;
                        hls = null;
                        fallbackReason = "HLS non disponibile (" + ex.Message + ").";
                        await StartProgressiveAsync(requestGeneration);
                    }
                }
                else
                {
                    fallbackReason = "nessun HLS per questo video.";
                    await StartProgressiveAsync(requestGeneration);
                }
                NotifyChanged();
                return requestGeneration == generation ? CurrentInfo : null;
            }
            catch (Exception ex)
            {
                if (requestGeneration == generation)
                {
                    errorText = "Impossibile riprodurre: " + ex.Message;
                    NotifyChanged();
                }
                return null;
            }
        }

        async Task StartProgressiveAsync(int requestGeneration)
        {
            try
            {
                var result = await ProgressivePlayer.GetAsync(CurrentInfo.VideoId);
                if (requestGeneration != generation) return;

                candidates = result.Formats.OrderBy(f => f.Itag == 18 ? 0 : 1)
                    .ThenByDescending(f => f.Height).ToList();
                if (candidates.Count == 0)
                    throw new ExtractionException("nessun flusso MP4 con video e audio (" +
                        string.Join("; ", result.Notes) + ")");

                candidateIndex = -1;
                TryNext();
            }
            catch (Exception ex)
            {
                if (requestGeneration != generation) return;
                errorText = "Impossibile riprodurre: " +
                    (fallbackReason == null ? "" : fallbackReason + " ") + ex.Message;
                NotifyChanged();
            }
        }

        void TryNext()
        {
            candidateIndex++;
            if (candidateIndex >= candidates.Count)
            {
                errorText = "Impossibile riprodurre questo video su questo telefono." +
                    (fallbackReason == null ? "" : " " + fallbackReason);
                NotifyChanged();
                return;
            }

            ProgressiveFormat format = candidates[candidateIndex];
            string label = string.IsNullOrEmpty(format.QualityLabel)
                ? format.Height + "p" : format.QualityLabel;
            string reason = candidateIndex == 0 ? "" :
                " Ripiego sul flusso " + (candidateIndex + 1) + ": " + fallbackReason;
            qualityText = "Qualità: " + label + ", MP4 progressivo (itag " + format.Itag +
                ", client " + format.Client + ")." +
                (fallbackReason == null ? "" : " " + fallbackReason) + reason;
            errorText = "";
            Player.Source = new Uri(format.Url);
            NotifyChanged();
        }

        void OnMediaFailed(object sender, ExceptionRoutedEventArgs e)
        {
            if (hls != null)
            {
                fallbackReason = "Il lettore HLS non si apre (" + e.ErrorMessage + ").";
                hls = null;
                errorText = "";
                StartProgressiveAsync(generation);
                NotifyChanged();
                return;
            }

            if (candidateIndex < 0 || candidateIndex >= candidates.Count) return;
            fallbackReason = "Il flusso itag " + candidates[candidateIndex].Itag +
                " non si apre (" + e.ErrorMessage + ").";
            TryNext();
        }

        void OnMediaOpened(object sender, RoutedEventArgs e)
        {
            if (pendingSeek.HasValue)
            {
                TimeSpan seek = pendingSeek.Value;
                pendingSeek = null;
                try { Player.Position = seek; }
                catch (Exception) { }
                resumed = true;
            }
            else if (!resumed && CurrentUrl != null)
            {
                resumed = true;
                long at = Positions.Get(CurrentUrl);
                if (at > 0)
                {
                    try { Player.Position = TimeSpan.FromSeconds(at); }
                    catch (Exception) { }
                }
            }
            NotifyChanged();
        }

        void OnCurrentStateChanged(object sender, RoutedEventArgs e)
        {
            NotifyChanged();
        }

        void OnHlsFailed(string message)
        {
            if (Player.Dispatcher.HasThreadAccess)
            {
                errorText = "Errore lettore: " + message;
                NotifyChanged();
                return;
            }

            var ignored = Player.Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                errorText = "Errore lettore: " + message;
                NotifyChanged();
            });
        }

        void OnPositionTimerTick(object sender, object e)
        {
            SavePosition();
        }

        public void SavePosition()
        {
            if (!HasVideo) return;
            try { Positions.Save(CurrentUrl, (long)Player.Position.TotalSeconds); }
            catch (Exception) { }
        }

        public void TogglePlayPause()
        {
            if (!HasVideo) return;
            if (Player.CurrentState == MediaElementState.Playing) Player.Pause();
            else Player.Play();
            NotifyChanged();
        }

        public async Task ChangeAudioAsync(AudioOption option)
        {
            if (option == null || hls == null || CurrentInfo == null) return;

            TimeSpan position = Player.Position;
            SavePositionAt(position);
            string language = option.Language;
            try
            {
                HlsSource replacement = await HlsSource.OpenAsync(CurrentInfo.HlsUrl, 720, language);
                if (CurrentInfo == null) return;

                replacement.Failed += OnHlsFailed;
                pendingSeek = position;
                hls = replacement;
                Player.SetMediaStreamSource(replacement.Source);
                qualityText = "Qualità: " + replacement.Label + ".";
                errorText = "";
                if (!string.IsNullOrEmpty(language))
                    ApplicationData.Current.LocalSettings.Values["audioLang"] = language;
                NotifyChanged();
            }
            catch (Exception ex)
            {
                errorText = "Impossibile cambiare traccia audio: " + ex.Message;
                NotifyChanged();
            }
        }

        void SavePositionAt(TimeSpan position)
        {
            if (CurrentUrl != null) Positions.Save(CurrentUrl, (long)position.TotalSeconds);
        }

        void AudioOptionCountChanged()
        {
            NotifyChanged();
        }

        public void Close()
        {
            generation++;
            SavePosition();
            try { Player.Stop(); }
            catch (Exception) { }
            try { Player.Source = null; }
            catch (Exception) { }

            hls = null;
            candidates = new List<ProgressiveFormat>();
            candidateIndex = -1;
            CurrentUrl = null;
            CurrentInfo = null;
            fallbackReason = null;
            qualityText = "";
            errorText = "";
            resumed = false;
            pendingSeek = null;
            NotifyChanged();
        }

        void NotifyChanged()
        {
            EventHandler handler = Changed;
            if (handler == null) return;

            if (Player.Dispatcher.HasThreadAccess)
            {
                handler(this, EventArgs.Empty);
                return;
            }

            var ignored = Player.Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                EventHandler currentHandler = Changed;
                if (currentHandler != null) currentHandler(this, EventArgs.Empty);
            });
        }
    }
}
