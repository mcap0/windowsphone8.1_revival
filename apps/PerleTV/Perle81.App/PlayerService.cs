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
using Windows.Media;
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
        readonly SystemMediaTransportControls transportControls;
        List<ProgressiveFormat> candidates = new List<ProgressiveFormat>();
        int candidateIndex = -1;
        int generation;
        bool resumed;
        TimeSpan? pendingSeek;
        HlsSource hls;
        string fallbackReason;
        string qualityText;
        string errorText;
        // keeps the screen on while a video plays: the normal standby suspended the app after a few minutes
        readonly Windows.System.Display.DisplayRequest screenOn = new Windows.System.Display.DisplayRequest();
        bool screenHeld;
        // robustness: the stream is opened again at the same place when it stops moving (stall, app resumed,
        // headphones in/out) instead of staying stuck or jumping "up and down"
        TimeSpan lastPosition;
        int stalledTicks;
        int reopens;            // in a row without progress: gives up after 3
        bool pauseAfterOpen;
        bool playingAtSuspend;
        bool reopening;
        bool released;          // stream closed while the background task plays (audio in sottofondo, casting)

        // Diagnostics: share/inbox/perletv.txt on the PC (tools/inbox), next to the cast lines.
        internal static void Log(string line)
        {
            try
            {
                var client = new Windows.Web.Http.HttpClient();
                var ignored = client.PostAsync(new Uri("http://192.168.1.148:8765/perletv"),
                    new Windows.Web.Http.HttpStringContent("player: " + line));
            }
            catch (Exception) { }
        }

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
                AudioCategory = Windows.UI.Xaml.Media.AudioCategory.BackgroundCapableMedia,
                Stretch = Windows.UI.Xaml.Media.Stretch.Uniform
            };
            Player.MediaOpened += OnMediaOpened;
            Player.MediaFailed += OnMediaFailed;
            Player.CurrentStateChanged += OnCurrentStateChanged;

            transportControls = SystemMediaTransportControls.GetForCurrentView();
            transportControls.IsEnabled = true;
            transportControls.IsPlayEnabled = true;
            transportControls.IsPauseEnabled = true;
            transportControls.IsStopEnabled = true;
            transportControls.ButtonPressed += OnTransportButtonPressed;

            try { Windows.Media.Devices.MediaDevice.DefaultAudioRenderDeviceChanged += OnAudioDeviceChanged; }
            catch (Exception) { }

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
            Log("open " + url + (released ? " (was released)" : ""));
            released = false;
            try { transportControls.IsEnabled = true; } catch (Exception) { }
            try { Player.AudioCategory = Windows.UI.Xaml.Media.AudioCategory.BackgroundCapableMedia; } catch (Exception) { }

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
                UpdateTransportMetadata();
                await AppState.AddToHistoryAsync(info);
                if (requestGeneration != generation) return null;

                string preferredAudio = ApplicationData.Current.LocalSettings.Values["audioLang"] as string;
                if (!string.IsNullOrEmpty(info.HlsUrl))
                {
                    try
                    {
                        HlsSource openedHls = await HlsSource.OpenAsync(info.HlsUrl, MaxHeight, preferredAudio);
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
            Log("media failed: " + e.ErrorMessage);
            reopening = false;
            pauseAfterOpen = false;
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
            Log("media opened" + (pendingSeek.HasValue ? " seek " + (long)pendingSeek.Value.TotalSeconds : ""));
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
            if (PerleCast.CastUi.IsActive || pauseAfterOpen) Player.Pause();
            pauseAfterOpen = false;
            reopening = false;
            stalledTicks = 0;
            NotifyChanged();
        }

        void OnCurrentStateChanged(object sender, RoutedEventArgs e)
        {
            try
            {
                if (Player.CurrentState == MediaElementState.Playing)
                    transportControls.PlaybackStatus = MediaPlaybackStatus.Playing;
                else if (Player.CurrentState == MediaElementState.Paused)
                    transportControls.PlaybackStatus = MediaPlaybackStatus.Paused;
                else if (Player.CurrentState == MediaElementState.Stopped || Player.CurrentState == MediaElementState.Closed)
                    transportControls.PlaybackStatus = MediaPlaybackStatus.Stopped;
            }
            catch (Exception) { }
            HoldScreen(Player.CurrentState == MediaElementState.Playing && !PerleCast.CastUi.IsActive);
            NotifyChanged();
        }

        void OnTransportButtonPressed(SystemMediaTransportControls sender, SystemMediaTransportControlsButtonPressedEventArgs e)
        {
            var button = e.Button;
            var ignored = Player.Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                try
                {
                    if (button == SystemMediaTransportControlsButton.Play) Player.Play();
                    else if (button == SystemMediaTransportControlsButton.Pause) Player.Pause();
                    else if (button == SystemMediaTransportControlsButton.Stop) Player.Stop();
                }
                catch (Exception) { }
            });
        }

        void UpdateTransportMetadata()
        {
            if (CurrentInfo == null) return;
            try
            {
                transportControls.DisplayUpdater.Type = MediaPlaybackType.Music;
                transportControls.DisplayUpdater.MusicProperties.Title = CurrentInfo.Name ?? "Perle TV";
                transportControls.DisplayUpdater.MusicProperties.Artist = CurrentInfo.UploaderName ?? "";
                transportControls.DisplayUpdater.Update();
            }
            catch (Exception) { }
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
            CheckStall();
        }

        /// <summary>Playing (or buffering) but the position has not moved for 10-20 s: open the stream again.</summary>
        void CheckStall()
        {
            if (!HasVideo || PerleCast.CastUi.IsActive || reopening || !resumed || pendingSeek.HasValue) { stalledTicks = 0; return; }
            var state = Player.CurrentState;
            if (state != MediaElementState.Playing && state != MediaElementState.Buffering) { stalledTicks = 0; return; }
            TimeSpan now = Player.Position;
            if (now != lastPosition)
            {
                lastPosition = now;
                stalledTicks = 0;
                if (now.TotalSeconds > 0) reopens = 0;
                return;
            }
            if (++stalledTicks < (state == MediaElementState.Playing ? 2 : 4)) return;
            if (reopens >= 3) return;  // the network is down: leave it, the user sees it
            reopens++;
            var ignored = ReopenAsync(true);
        }

        void OnAudioDeviceChanged(object sender, Windows.Media.Devices.DefaultAudioRenderDeviceChangedEventArgs e)
        {
            // headphones in/out: the open stream often stalls on the new output
            var ignored = Player.Dispatcher.RunAsync(CoreDispatcherPriority.Normal, async () =>
            {
                await Task.Delay(800);
                if (HasVideo && !PerleCast.CastUi.IsActive && resumed) await ReopenAsync(IsPlaying);
            });
        }

        public void OnAppSuspending()
        {
            playingAtSuspend = IsPlaying;
            Log("suspending, " + Player.CurrentState + (released ? ", released" : ""));
            SavePosition();
        }

        /// <summary>Back from the background: a suspended media source rarely resumes cleanly, open it again.</summary>
        public void OnAppResumed()
        {
            Log("resumed, " + Player.CurrentState + (released ? ", released" : "") + (HasVideo ? "" : ", no video"));
            if (!HasVideo || PerleCast.CastUi.IsActive || released) return;
            var ignored = ReopenAsync(playingAtSuspend);
        }

        /// <summary>The background task needs the audio: the phone player lets the stream go (pausing it kept it
        /// and the task's player never opened). Restore() opens it again where the task got.</summary>
        public void ReleaseForBackground()
        {
            if (released || !HasVideo) return;
            SavePosition();
            released = true;
            generation++;
            Log("released for the background task");
            try { Player.Stop(); } catch (Exception) { }
            try { Player.Source = null; } catch (Exception) { }
            // the phone's player must not claim background audio while the task plays it
            try { Player.AudioCategory = Windows.UI.Xaml.Media.AudioCategory.ForegroundOnlyMedia; } catch (Exception) { }
            try { transportControls.IsEnabled = false; } catch (Exception) { }
            HoldScreen(false);
        }

        public void Restore(bool play)
        {
            if (!released) return;
            released = false;
            Log("restore, play " + play);
            try { Player.AudioCategory = Windows.UI.Xaml.Media.AudioCategory.BackgroundCapableMedia; } catch (Exception) { }
            try { transportControls.IsEnabled = true; } catch (Exception) { }
            if (HasVideo) { var ignored = ReopenAsync(play); }
        }

        public bool IsReleased { get { return released; } }

        /// <summary>Opens the current stream again at the current (or last saved) position.</summary>
        public async Task ReopenAsync(bool play)
        {
            if (!HasVideo || reopening) return;
            Log("reopen, play " + play);
            TimeSpan position = Player.Position;
            if (position.TotalSeconds < 1) position = TimeSpan.FromSeconds(Positions.Get(CurrentUrl));
            if (position.TotalSeconds < 1 && lastPosition.TotalSeconds > 1) position = lastPosition;
            SavePositionAt(position);
            int requestGeneration = generation;
            reopening = true;
            try
            {
                if (hls != null)
                {
                    HlsSource replacement = await HlsSource.OpenAsync(CurrentInfo.HlsUrl, MaxHeight,
                        SelectedAudioOption == null ? null : SelectedAudioOption.Language);
                    if (requestGeneration != generation || CurrentInfo == null) { reopening = false; return; }
                    replacement.Failed += OnHlsFailed;
                    pendingSeek = position;
                    pauseAfterOpen = !play;
                    hls = replacement;
                    Player.SetMediaStreamSource(replacement.Source);
                }
                else if (candidateIndex >= 0 && candidateIndex < candidates.Count)
                {
                    pendingSeek = position;
                    pauseAfterOpen = !play;
                    Player.Source = new Uri(candidates[candidateIndex].Url);
                }
                else reopening = false;
            }
            catch (Exception ex)
            {
                reopening = false;
                errorText = "Riapertura non riuscita: " + ex.Message;
                NotifyChanged();
            }
        }

        void HoldScreen(bool hold)
        {
            if (hold == screenHeld) return;
            try
            {
                if (hold) screenOn.RequestActive();
                else screenOn.RequestRelease();
                screenHeld = hold;
            }
            catch (Exception) { }
        }

        public void SavePosition()
        {
            // while casting the TV saves the position, with "audio in sottofondo" the background task does
            if (!HasVideo || PerleCast.CastUi.IsActive || PerleCast.BackgroundAudio.IsActive) return;
            // before MediaOpened has applied the resume seek the player says 0: saving it lost the place
            // ("perle forgets minutes of videos and restarts from zero")
            if (!resumed || pendingSeek.HasValue) return;
            try
            {
                long seconds = (long)Player.Position.TotalSeconds;
                if (seconds < 5 && Positions.Get(CurrentUrl) > 30) return;  // seek still in flight
                Positions.Save(CurrentUrl, seconds);
            }
            catch (Exception) { }
        }

        public void TogglePlayPause()
        {
            if (!HasVideo) return;
            if (released && !PerleCast.CastUi.IsActive && !PerleCast.BackgroundAudio.IsActive) { Restore(true); return; }
            if (Player.CurrentState == MediaElementState.Playing) Player.Pause();
            else Player.Play();
            NotifyChanged();
        }

        /// <summary>Highest resolution played on the phone (setting "maxHeight", 720 by default).</summary>
        public static int MaxHeight
        {
            get
            {
                object value = ApplicationData.Current.LocalSettings.Values["maxHeight"];
                return value is int ? (int)value : 720;
            }
        }

        public async Task ChangeQualityAsync(int height)
        {
            ApplicationData.Current.LocalSettings.Values["maxHeight"] = height;
            if (hls == null || CurrentInfo == null) return;

            TimeSpan position = Player.Position;
            SavePositionAt(position);
            try
            {
                HlsSource replacement = await HlsSource.OpenAsync(CurrentInfo.HlsUrl, height,
                    SelectedAudioOption == null ? null : SelectedAudioOption.Language);
                if (CurrentInfo == null) return;

                replacement.Failed += OnHlsFailed;
                pendingSeek = position;
                hls = replacement;
                Player.SetMediaStreamSource(replacement.Source);
                qualityText = "Qualità: " + replacement.Label + ".";
                errorText = "";
                NotifyChanged();
            }
            catch (Exception ex)
            {
                errorText = "Impossibile cambiare qualità: " + ex.Message;
                NotifyChanged();
            }
        }

        public async Task ChangeAudioAsync(AudioOption option)
        {
            if (option == null || hls == null || CurrentInfo == null) return;

            TimeSpan position = Player.Position;
            SavePositionAt(position);
            string language = option.Language;
            try
            {
                HlsSource replacement = await HlsSource.OpenAsync(CurrentInfo.HlsUrl, MaxHeight, language);
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
            reopening = false;
            pauseAfterOpen = false;
            stalledTicks = 0;
            reopens = 0;
            lastPosition = TimeSpan.Zero;
            HoldScreen(false);
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
