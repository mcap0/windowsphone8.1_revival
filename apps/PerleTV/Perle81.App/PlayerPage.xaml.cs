/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Video details, subtitles, audio selection and full-screen UI. Playback itself belongs to PlayerService.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Threading.Tasks;
using Perle.Extractor;
using Windows.Graphics.Display;
using Windows.Storage;
using Windows.UI.Core;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class PlayerPage : Page, IBackHandler
    {
        static readonly HttpClient subtitleHttp = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };

        string url;
        bool fullscreen;
        DispatcherTimer subtitleTimer;
        List<SubtitleCue> subtitleCues = new List<SubtitleCue>();
        SubtitleTrack selectedSubtitleTrack;
        bool selectedSubtitleTranslated;
        bool subtitlesOff = true;
        int subtitleRequestVersion;
        PlayerService service;

        sealed class SubtitleChoice
        {
            public string Label;
            public SubtitleTrack Track;
            public bool IsTranslated;
            public bool IsOff;
        }

        public PlayerPage()
        {
            InitializeComponent();
            SizeChanged += OnSizeChanged;

            subtitleTimer = new DispatcherTimer();
            subtitleTimer.Interval = TimeSpan.FromMilliseconds(200);
            subtitleTimer.Tick += OnSubtitleTimerTick;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            // one video page only: Back from a suggested video goes to the list (mini player), not the video before
            for (int i = Frame.BackStack.Count - 1; i >= 0; i--)
                if (Frame.BackStack[i].SourcePageType == typeof(PlayerPage)) Frame.BackStack.RemoveAt(i);
            service = PlayerService.Current;
            url = e.Parameter as string;
            subtitleTimer.Start();

            if (service == null || string.IsNullOrEmpty(url))
            {
                Busy.Visibility = Visibility.Collapsed;
                ErrorText.Text = "Impossibile aprire il video.";
                return;
            }

            service.Changed += OnPlayerServiceChanged;
            Application.Current.Resuming -= OnAppResuming;
            Application.Current.Resuming += OnAppResuming;
            PerleCast.BackgroundAudio.StatusChanged += OnBackgroundAudioStatus;
            if (PerleCast.BackgroundAudio.IsActive &&
                !string.Equals(PerleCast.BackgroundAudio.VideoUrl, url, StringComparison.OrdinalIgnoreCase))
                PerleCast.BackgroundAudio.Stop();  // another video: its audio stops
            if (ShellPage.Current != null)
                ShellPage.Current.SetPlayerPageVisible(true, VideoArea, fullscreen);

            Busy.Visibility = Visibility.Visible;
            try
            {
                StreamInfo info;
                if (string.Equals(service.CurrentUrl, url, StringComparison.OrdinalIgnoreCase) &&
                    service.CurrentInfo != null)
                {
                    info = service.CurrentInfo;
                }
                else
                {
                    info = await service.OpenAsync(url);
                }

                if (info == null)
                {
                    ErrorText.Text = string.IsNullOrEmpty(service.ErrorText)
                        ? "Impossibile riprodurre questo video." : service.ErrorText;
                    return;
                }

                TitleText.Text = info.Name ?? "";
                MetaText.Text = (info.UploaderName ?? "") +
                    (info.ViewCount >= 0 ? " · " + Format.Views(info.ViewCount) : "");
                DescriptionText.Text = info.Description ?? "";
                ApplySavedSubtitleChoice();
                var related = LoadRelatedAsync(url);
                LaterCommand.Label = await WatchLater.ContainsAsync(info.Url ?? url) ? "togli da guarda dopo" : "guarda dopo";
                ChannelCommand.Visibility = info.UploaderUrl == null ? Visibility.Collapsed : Visibility.Visible;
                SubscribeCommand.Visibility = ChannelCommand.Visibility;
                if (info.UploaderUrl != null) await ShowSubscribedAsync(info.UploaderUrl);
                // while casting, the video opened goes to the TV instead of the phone player
                if (!string.IsNullOrEmpty(info.HlsUrl) && PerleCast.CastUi.PlayOnTvIfCasting(CastVideoFor(info, -1)))
                    service.Player.Pause();
                if (PerleCast.BackgroundAudio.IsActive) service.ReleaseForBackground();  // its audio is playing in the background
                UpdateBackgroundAudioCommand();
                UpdatePlayerServiceUi();
                UpdateShellBounds();
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Impossibile riprodurre: " + ex.Message;
            }
            finally
            {
                Busy.Visibility = Visibility.Collapsed;
            }
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            subtitleTimer.Stop();
            subtitleRequestVersion++;
            if (service != null)
            {
                service.SavePosition();
                service.Changed -= OnPlayerServiceChanged;
            }
            PerleCast.BackgroundAudio.StatusChanged -= OnBackgroundAudioStatus;
            Application.Current.Resuming -= OnAppResuming;
            if (fullscreen) ExitFullscreen();
            if (ShellPage.Current != null)
                ShellPage.Current.SetPlayerPageVisible(false, null, false);
            base.OnNavigatedFrom(e);
        }

        void OnPlayerServiceChanged(object sender, EventArgs e)
        {
            if (!Dispatcher.HasThreadAccess)
            {
                var ignored = Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal,
                    UpdatePlayerServiceUi);
                return;
            }
            UpdatePlayerServiceUi();
        }

        void UpdatePlayerServiceUi()
        {
            if (service == null) return;
            QualityText.Text = service.QualityText;
            ErrorText.Text = service.ErrorText;
            // the video could not be played (e.g. the reopen after a resume): leave full screen to show why
            if (fullscreen && !string.IsNullOrEmpty(service.ErrorText)) ExitFullscreen();
            AudioCommand.Visibility = service.AudioOptionCount > 1
                ? Visibility.Visible : Visibility.Collapsed;
            UpdateSubtitleDisplay();
        }

        void OnAudioCommandClick(object sender, RoutedEventArgs e)
        {
            if (service == null || service.AudioOptionCount <= 1) return;

            // a full-page list: a MenuFlyout does not scroll when there are many tracks
            List<AudioOption> options = service.AudioOptions.ToList();
            var flyout = new ListPickerFlyout();
            Windows.UI.Xaml.Controls.Primitives.PickerFlyoutBase.SetTitle(flyout, "TRACCIA AUDIO");
            flyout.ItemsSource = options.Select(o => (IsCurrentAudio(o) ? "✓ " : "") + AudioLabel(o)).ToList();
            flyout.ItemsPicked += async (s, args) =>
            {
                int index = s.SelectedIndex;
                if (index >= 0 && index < options.Count && service != null)
                    await service.ChangeAudioAsync(options[index]);
            };
            var shown = flyout.ShowAtAsync(VideoArea);
        }

        bool IsCurrentAudio(AudioOption option)
        {
            if (service == null || service.SelectedAudioOption == null || option == null) return false;
            return string.Equals(option.Uri, service.SelectedAudioOption.Uri,
                StringComparison.OrdinalIgnoreCase);
        }

        static string AudioLabel(AudioOption option)
        {
            string name = string.IsNullOrWhiteSpace(option.Name) ? "Traccia audio" : option.Name;
            if (!string.IsNullOrWhiteSpace(option.Language) &&
                !string.Equals(name, option.Language, StringComparison.OrdinalIgnoreCase))
                name += " (" + option.Language + ")";
            return name;
        }

        void ApplySavedSubtitleChoice()
        {
            object stored = ApplicationData.Current.LocalSettings.Values["subLang"];
            string language = stored as string;
            if (string.IsNullOrEmpty(language) || language == "off")
            {
                SetSubtitlesOff(false);
                return;
            }

            SubtitleTrack track = FindSubtitleTrack(language);
            if (track != null)
            {
                LoadSubtitleTrack(track, false);
                return;
            }

            if (IsItalian(language) && !HasItalianTrack())
                LoadTranslatedItalian(false);
            else
                SetSubtitlesOff(false);
        }

        void OnSubtitleCommandClick(object sender, RoutedEventArgs e)
        {
            var choices = new List<SubtitleChoice>
            {
                new SubtitleChoice { Label = "nessuno", IsOff = true }
            };

            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info != null && info.Subtitles != null)
            {
                foreach (SubtitleTrack track in info.Subtitles)
                {
                    string label = string.IsNullOrWhiteSpace(track.Name)
                        ? track.LanguageTag ?? "Sottotitoli" : track.Name;
                    if (track.IsAutoGenerated) label += " (automatici)";
                    choices.Add(new SubtitleChoice { Label = label, Track = track });
                }
                if (!HasItalianTrack())
                    choices.Add(new SubtitleChoice { Label = "italiano (traduzione automatica)", IsTranslated = true });
            }

            // a full-page list: a MenuFlyout does not scroll when there are many languages
            var flyout = new ListPickerFlyout();
            Windows.UI.Xaml.Controls.Primitives.PickerFlyoutBase.SetTitle(flyout, "SOTTOTITOLI");
            flyout.ItemsSource = choices.Select(c => (IsCurrentSubtitle(c) ? "✓ " : "") + c.Label).ToList();
            flyout.ItemsPicked += (s, args) =>
            {
                int index = s.SelectedIndex;
                if (index >= 0 && index < choices.Count) OnSubtitleChosen(choices[index]);
            };
            var shown = flyout.ShowAtAsync(VideoArea);
        }

        async void OnSubtitleChosen(SubtitleChoice choice)
        {
            if (choice == null) return;

            if (choice.IsOff)
            {
                ApplicationData.Current.LocalSettings.Values["subLang"] = "off";
                SetSubtitlesOff(true);
            }
            else if (choice.IsTranslated)
            {
                ApplicationData.Current.LocalSettings.Values["subLang"] = "it";
                await LoadTranslatedItalian(true);
            }
            else if (choice.Track != null)
            {
                string language = SubtitleLanguageKey(choice.Track);
                ApplicationData.Current.LocalSettings.Values["subLang"] = language;
                await LoadSubtitleTrack(choice.Track, true);
            }
        }

        async Task LoadSubtitleTrack(SubtitleTrack track, bool reportErrors)
        {
            int request = ++subtitleRequestVersion;
            selectedSubtitleTrack = track;
            selectedSubtitleTranslated = false;
            subtitlesOff = false;
            subtitleCues = new List<SubtitleCue>();
            UpdateSubtitleDisplay();
            UpdateShellSubtitle();

            try
            {
                string subtitleUrl = track.UrlFor("vtt");
                string text = await subtitleHttp.GetStringAsync(subtitleUrl);
                if (request != subtitleRequestVersion) return;
                subtitleCues = SubtitleCues.Parse(text);
                UpdateSubtitleDisplay();
            }
            catch (Exception ex)
            {
                if (request == subtitleRequestVersion && reportErrors)
                    ErrorText.Text = "Impossibile caricare i sottotitoli: " + ex.Message;
            }
        }

        async Task LoadTranslatedItalian(bool reportErrors)
        {
            int request = ++subtitleRequestVersion;
            selectedSubtitleTrack = null;
            selectedSubtitleTranslated = true;
            subtitlesOff = false;
            subtitleCues = new List<SubtitleCue>();
            UpdateSubtitleDisplay();
            UpdateShellSubtitle();

            try
            {
                StreamInfo info = service == null ? null : service.CurrentInfo;
                SubtitleTrack sourceTrack = info == null || info.Subtitles == null
                    ? null
                    : info.Subtitles.FirstOrDefault(t => !string.IsNullOrEmpty(t.BaseUrl));
                if (sourceTrack == null) throw new InvalidOperationException("nessuna traccia da tradurre");
                string subtitleUrl = sourceTrack.UrlFor("vtt") + "&tlang=it";
                string text = await subtitleHttp.GetStringAsync(subtitleUrl);
                if (request != subtitleRequestVersion) return;
                subtitleCues = SubtitleCues.Parse(text);
                UpdateSubtitleDisplay();
            }
            catch (Exception ex)
            {
                if (request == subtitleRequestVersion && reportErrors)
                    ErrorText.Text = "Impossibile caricare la traduzione: " + ex.Message;
            }
        }

        void SetSubtitlesOff(bool invalidateRequest)
        {
            if (invalidateRequest) subtitleRequestVersion++;
            selectedSubtitleTrack = null;
            selectedSubtitleTranslated = false;
            subtitlesOff = true;
            subtitleCues = new List<SubtitleCue>();
            UpdateSubtitleDisplay();
            UpdateShellSubtitle();
        }

        SubtitleTrack FindSubtitleTrack(string language)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info == null || info.Subtitles == null) return null;

            SubtitleTrack exact = info.Subtitles.FirstOrDefault(t =>
                string.Equals(SubtitleLanguageKey(t), language, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(t.LanguageTag, language, StringComparison.OrdinalIgnoreCase));
            if (exact != null) return exact;

            string primary = PrimaryLanguage(language);
            return info.Subtitles.FirstOrDefault(t =>
                !string.IsNullOrEmpty(t.LanguageTag) &&
                string.Equals(PrimaryLanguage(t.LanguageTag), primary, StringComparison.OrdinalIgnoreCase));
        }

        bool HasItalianTrack()
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            return info != null && info.Subtitles != null &&
                info.Subtitles.Any(t => IsItalian(t.LanguageTag));
        }

        bool IsCurrentSubtitle(SubtitleChoice choice)
        {
            if (choice.IsOff) return subtitlesOff;
            if (choice.IsTranslated) return selectedSubtitleTranslated && !subtitlesOff;
            return !subtitlesOff && !selectedSubtitleTranslated &&
                object.ReferenceEquals(choice.Track, selectedSubtitleTrack);
        }

        static string SubtitleLanguageKey(SubtitleTrack track)
        {
            if (!string.IsNullOrWhiteSpace(track.LanguageTag)) return track.LanguageTag;
            return "name:" + (track.Name ?? "");
        }

        static bool IsItalian(string language)
        {
            return !string.IsNullOrEmpty(language) &&
                string.Equals(PrimaryLanguage(language), "it", StringComparison.OrdinalIgnoreCase);
        }

        static string PrimaryLanguage(string language)
        {
            if (language == null) return "";
            int separator = language.IndexOf('-');
            return separator < 0 ? language : language.Substring(0, separator);
        }

        void OnSubtitleTimerTick(object sender, object e)
        {
            UpdateSubtitleDisplay();
        }

        void UpdateSubtitleDisplay()
        {
            if (subtitlesOff || subtitleCues == null || subtitleCues.Count == 0 || service == null)
            {
                UpdateShellSubtitle();
                return;
            }

            string text;
            try { text = SubtitleCues.TextAt(subtitleCues, service.Player.Position); }
            catch { text = ""; }

            if (ShellPage.Current != null)
                ShellPage.Current.SetSubtitle(text, !string.IsNullOrEmpty(text));
        }

        void UpdateShellSubtitle()
        {
            if (ShellPage.Current != null)
                ShellPage.Current.SetSubtitle("", false);
        }

        void OnSizeChanged(object sender, SizeChangedEventArgs e)
        {
            bool landscape = ActualWidth > ActualHeight;
            if (landscape && !fullscreen) EnterFullscreen();
            else if (!landscape && fullscreen) ExitFullscreen();
            Layout();
        }

        void Layout()
        {
            if (fullscreen)
            {
                VideoArea.Height = ActualHeight;
                Grid.SetRowSpan(VideoArea, 2);
            }
            else
            {
                VideoArea.Height = ActualWidth * 9 / 16;
                Grid.SetRowSpan(VideoArea, 1);
            }
            UpdateShellBounds();
        }

        void UpdateShellBounds()
        {
            if (ShellPage.Current != null)
                ShellPage.Current.UpdatePlayerPageBounds(VideoArea, fullscreen);
        }

        void OnDownloadAudio(object sender, RoutedEventArgs e)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info == null) return;
            DownloadService.Enqueue(info.Url, info.Name, info.UploaderName);
            ErrorText.Text = "Scarica audio: aggiunto alla coda (scheda Download).";
        }

        /// <summary>"audio in sottofondo": the audio goes on in the background task (Start, screen locked); closing
        /// Perle TV stops it. Again from the menu ("torna al video"): the video continues where the audio got.</summary>
        async void OnBackgroundAudioClick(object sender, RoutedEventArgs e)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info == null) return;
            if (PerleCast.BackgroundAudio.IsActive)
            {
                PerleCast.BackgroundAudio.Stop();
                return;
            }
            if (PerleCast.CastUi.IsActive)
            {
                ErrorText.Text = "Durante la trasmissione l'audio è sul TV.";
                return;
            }
            double position = 0;
            try { position = service.Player.Position.TotalSeconds; } catch { }
            if (position < 1) position = Positions.Get(info.Url);
            service.ReleaseForBackground();
            ErrorText.Text = "Audio in sottofondo…";
            try
            {
                string source = await DownloadedAudioPathAsync(info.Url);
                if (source != null) source = "file:" + source;
                else
                {
                    var full = await YoutubeStream.GetInfoAsync(info.Url);
                    // the language heard in the player (dubbed videos have one AAC track per language)
                    string lang = service.SelectedAudioOption != null ? service.SelectedAudioOption.Language
                        : Windows.Storage.ApplicationData.Current.LocalSettings.Values["audioLang"] as string;
                    var aac = full.AdaptiveFormats.Where(f => f.Url != null && f.IsAac).ToList();
                    var audio = (string.IsNullOrEmpty(lang) ? null : aac.Where(f => f.AudioTrackId != null &&
                            f.AudioTrackId.StartsWith(lang, StringComparison.OrdinalIgnoreCase))
                            .OrderByDescending(f => f.Itag == 140).ThenByDescending(f => f.Bitrate).FirstOrDefault())
                        ?? aac.Where(f => f.IsDefaultAudioTrack).OrderByDescending(f => f.Itag == 140)
                            .ThenByDescending(f => f.Bitrate).FirstOrDefault()
                        ?? YoutubeStream.BestAudio(full);
                    PlayerService.Log("background audio itag " + (audio == null ? 0 : audio.Itag) + " track " +
                        (audio == null ? "-" : audio.AudioTrackId) + ", wanted " + lang);
                    if (audio == null || audio.Url == null) throw new InvalidOperationException("nessun flusso audio");
                    source = audio.Url;
                }
                PerleCast.BackgroundAudio.Play(source, info.Url, info.Name ?? "", info.UploaderName ?? "", position);
                UpdateBackgroundAudioCommand();
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Audio in sottofondo non riuscito: " + ex.Message;
            }
        }

        /// <summary>The file of "Scarica audio" for this video, if it is complete.</summary>
        static async Task<string> DownloadedAudioPathAsync(string videoUrl)
        {
            var item = DownloadService.Items.FirstOrDefault(i => i.State == DownloadState.Done &&
                !string.IsNullOrEmpty(i.SavedRelativePath) && string.Equals(i.Url, videoUrl, StringComparison.OrdinalIgnoreCase));
            if (item == null) return null;
            try
            {
                var perle = await KnownFolders.MusicLibrary.GetFolderAsync("Perle");
                var parts = item.SavedRelativePath.Split('\\');
                var folder = await perle.GetFolderAsync(parts[0]);
                return (await folder.GetFileAsync(parts[1])).Path;
            }
            catch (Exception) { return null; }
        }

        void OnBackgroundAudioStatus(object sender, string status)
        {
            UpdateBackgroundAudioCommand();
            if (status == "playing")
            {
                ErrorText.Text = "Audio in sottofondo: puoi uscire con Start o bloccare lo schermo (chiudendo Perle TV si ferma).";
                return;
            }
            if (status.StartsWith("error"))
                ErrorText.Text = "Audio in sottofondo: " + status;
            else
                ErrorText.Text = "";
            // back to the video where the audio got (the task saved the position)
            if (service != null && service.HasVideo && !PerleCast.CastUi.IsActive &&
                string.Equals(service.CurrentUrl, PerleCast.BackgroundAudio.VideoUrl, StringComparison.OrdinalIgnoreCase))
                service.Restore(status == "stopped");
        }

        void UpdateBackgroundAudioCommand()
        {
            BackgroundAudioCommand.Label = PerleCast.BackgroundAudio.IsActive ? "torna al video" : "audio in sottofondo";
        }

        async Task ShowSubscribedAsync(string channelUrl)
        {
            SubscribeCommand.Label = await AppState.IsSubscribedAsync(channelUrl) ? "annulla iscrizione" : "iscriviti al canale";
        }

        async void OnSubscribeCommandClick(object sender, RoutedEventArgs e)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info == null || info.UploaderUrl == null) return;
            try
            {
                bool on = !await AppState.IsSubscribedAsync(info.UploaderUrl);
                await AppState.SetSubscribedAsync(info.UploaderUrl, info.UploaderName, on);
                await ShowSubscribedAsync(info.UploaderUrl);
            }
            catch (Exception) { }
        }

        void OnChannelCommandClick(object sender, RoutedEventArgs e)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info != null && info.UploaderUrl != null) Frame.Navigate(typeof(ChannelPage), info.UploaderUrl);
        }

        async void OnLaterCommandClick(object sender, RoutedEventArgs e)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info == null) return;
            if (await WatchLater.ContainsAsync(info.Url))
            {
                await WatchLater.RemoveAsync(info.Url);
                LaterCommand.Label = "guarda dopo";
                ErrorText.Text = "Tolto da Guarda dopo.";
            }
            else
            {
                await WatchLater.AddAsync(info.Url, info.Name, info.UploaderName, info.Duration);
                LaterCommand.Label = "togli da guarda dopo";
                ErrorText.Text = "Aggiunto a Guarda dopo (scheda nella pagina principale).";
            }
        }

        async void OnRelatedHolding(object sender, Windows.UI.Xaml.Input.HoldingRoutedEventArgs e)
        {
            if (e.HoldingState != Windows.UI.Input.HoldingState.Started) return;
            var source = sender as FrameworkElement;
            var item = source?.DataContext as VideoItem;
            if (item == null) return;
            var menu = new MenuFlyout();
            menu.Items.Add(await WatchLater.MenuItemAsync(item, text => ErrorText.Text = text));
            menu.ShowAt(source);
        }

        void OnQualityCommandClick(object sender, RoutedEventArgs e)
        {
            var flyout = new MenuFlyout();
            foreach (int height in new[] { 1080, 720, 480 })
            {
                var item = new MenuFlyoutItem
                {
                    Text = (PlayerService.MaxHeight == height ? "✓ " : "") + "fino a " + height + "p",
                    Tag = height
                };
                item.Click += async (s, a) =>
                {
                    if (service != null) await service.ChangeQualityAsync((int)((MenuFlyoutItem)s).Tag);
                };
                flyout.Items.Add(item);
            }
            flyout.ShowAt(VideoArea);
        }

        void OnCastClick(object sender, RoutedEventArgs e)
        {
            StreamInfo info = service == null ? null : service.CurrentInfo;
            if (info == null || string.IsNullOrEmpty(info.HlsUrl))
            {
                if (PerleCast.CastUi.IsActive) PerleCast.CastUi.Reopen();
                else ErrorText.Text = "Trasmissione non disponibile per questo video.";
                return;
            }

            double position = -1;
            try { if (service.Player.Position.TotalSeconds > 0) position = service.Player.Position.TotalSeconds; }
            catch { }
            service.Player.Pause();
            PerleCast.CastUi.Show(CastVideoFor(info, position));
        }

        /// <summary>The video for the TV, with the audio and subtitles chosen on the phone.</summary>
        PerleCast.CastVideo CastVideoFor(StreamInfo info, double startSeconds)
        {
            SubtitleTrack sourceTrack = info.Subtitles == null ? null
                : info.Subtitles.FirstOrDefault(t => !string.IsNullOrEmpty(t.BaseUrl));
            string subtitleUrl = "";
            if (!subtitlesOff && selectedSubtitleTrack != null)
                subtitleUrl = selectedSubtitleTrack.UrlFor("vtt");
            else if (!subtitlesOff && selectedSubtitleTranslated && sourceTrack != null)
                subtitleUrl = sourceTrack.UrlFor("vtt") + "&tlang=it";
            return CastVideoFor(info, url, startSeconds, subtitleUrl);
        }

        static PerleCast.CastVideo CastVideoFor(StreamInfo info, string videoUrl, double startSeconds, string subtitleUrl)
        {
            var video = new PerleCast.CastVideo
            {
                VideoUrl = videoUrl ?? "",
                Title = info.Name ?? "",
                Uploader = info.UploaderName ?? "",
                HlsUrl = info.HlsUrl,
                AudioLanguage = ApplicationData.Current.LocalSettings.Values["audioLang"] as string ?? "",
                SubtitleUrl = subtitleUrl ?? "",
                StartSeconds = startSeconds
            };

            if (info.Subtitles != null)
            {
                foreach (SubtitleTrack track in info.Subtitles)
                {
                    string label = string.IsNullOrWhiteSpace(track.Name)
                        ? track.LanguageTag ?? "Sottotitoli" : track.Name;
                    if (track.IsAutoGenerated) label += " (automatici)";
                    video.SubtitleLabels.Add(label);
                    video.SubtitleUrls.Add(track.UrlFor("vtt"));
                }
                SubtitleTrack sourceTrack = info.Subtitles.FirstOrDefault(t => !string.IsNullOrEmpty(t.BaseUrl));
                if (sourceTrack != null && !info.Subtitles.Any(t => IsItalian(t.LanguageTag)))
                {
                    video.SubtitleLabels.Add("italiano (traduzione automatica)");
                    video.SubtitleUrls.Add(sourceTrack.UrlFor("vtt") + "&tlang=it");
                }
            }
            return video;
        }

        /// <summary>Adds a video of a list to the TV queue, with the subtitles of the saved choice.</summary>
        public static async Task<bool> AddToTvQueueAsync(string videoUrl)
        {
            if (!PerleCast.CastUi.IsActive) return false;
            StreamInfo info = await YoutubeStream.GetInfoAsync(videoUrl);
            if (info == null || string.IsNullOrEmpty(info.HlsUrl)) return false;

            string language = ApplicationData.Current.LocalSettings.Values["subLang"] as string;
            string subtitleUrl = "";
            if (!string.IsNullOrEmpty(language) && language != "off" && info.Subtitles != null)
            {
                SubtitleTrack track = info.Subtitles.FirstOrDefault(t =>
                    string.Equals(PrimaryLanguage(t.LanguageTag), PrimaryLanguage(language), StringComparison.OrdinalIgnoreCase));
                SubtitleTrack sourceTrack = info.Subtitles.FirstOrDefault(t => !string.IsNullOrEmpty(t.BaseUrl));
                if (track != null) subtitleUrl = track.UrlFor("vtt");
                else if (IsItalian(language) && sourceTrack != null) subtitleUrl = sourceTrack.UrlFor("vtt") + "&tlang=it";
            }
            PerleCast.CastUi.AddToQueue(CastVideoFor(info, videoUrl, -1, subtitleUrl));
            return true;
        }

        void OnFullscreenClick(object sender, RoutedEventArgs e)
        {
            ToggleFullscreen();
        }

        // only reached with no video on screen (the video's own layer in ShellPage takes the taps)
        void OnVideoTapped(object sender, Windows.UI.Xaml.Input.TappedRoutedEventArgs e)
        {
            if (fullscreen) ExitFullscreen();
        }

        /// <summary>
        /// Back from the background: the system may have shown the status bar again and the stream is reopened.
        /// Full screen follows the phone's orientation again, and the controls show so there is always a way out.
        /// </summary>
        void OnAppResuming(object sender, object e)
        {
            var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                bool landscape = ActualWidth > ActualHeight;
                PlayerService.Log("page resumed, fullscreen " + fullscreen + ", landscape " + landscape);
                if (fullscreen && !landscape) ExitFullscreen();
                else if (fullscreen) { var hidden = StatusBar.GetForCurrentView().HideAsync(); Layout(); }
                if (ShellPage.Current != null) ShellPage.Current.ShowVideoControls();
            });
        }

        string relatedFor;

        /// <summary>"consigliati" under the description: YouTube's suggestions for this video.</summary>
        async Task LoadRelatedAsync(string videoUrl)
        {
            if (relatedFor == videoUrl) return;
            relatedFor = videoUrl;
            RelatedList.ItemsSource = null;
            RelatedHeader.Visibility = Visibility.Collapsed;
            try
            {
                var found = await YoutubeStream.GetRelatedAsync(videoUrl);
                if (relatedFor != videoUrl) return;
                RelatedList.ItemsSource = found.Select(v => new VideoItem(Perle.PerleVideo.From(v))).ToList();
                RelatedHeader.Visibility = found.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            }
            catch (Exception)
            {
                if (relatedFor == videoUrl) relatedFor = null;  // try again next time
            }
        }

        void OnRelatedTapped(object sender, Windows.UI.Xaml.Input.TappedRoutedEventArgs e)
        {
            var item = (sender as FrameworkElement)?.DataContext as VideoItem;
            if (item != null) Frame.Navigate(typeof(PlayerPage), item.Video.Url);
        }

        /// <summary>Double tap on the video: left third -10 s, right third +10 s, middle = full screen.
        /// Returns the seconds moved, 0 when it toggled full screen.</summary>
        public int DoubleTapAt(double x, double width)
        {
            int step = x < width / 3 ? -10 : x > width * 2 / 3 ? 10 : 0;
            if (step == 0 || service == null || !service.HasVideo || PerleCast.CastUi.IsActive)
            {
                ToggleFullscreen();
                return 0;
            }
            try
            {
                var player = service.Player;
                TimeSpan to = player.Position + TimeSpan.FromSeconds(step);
                if (to < TimeSpan.Zero) to = TimeSpan.Zero;
                if (player.NaturalDuration.HasTimeSpan && to > player.NaturalDuration.TimeSpan)
                    to = player.NaturalDuration.TimeSpan - TimeSpan.FromSeconds(1);
                player.Position = to;
            }
            catch (Exception) { }
            return step;
        }

        public void ToggleFullscreen()
        {
            if (fullscreen) ExitFullscreen(); else EnterFullscreen();
        }

        public bool HandleBack()
        {
            if (!fullscreen) return false;
            ExitFullscreen();
            return true;
        }

        void EnterFullscreen()
        {
            PlayerService.Log("fullscreen on");
            fullscreen = true;
            DisplayInformation.AutoRotationPreferences =
                DisplayOrientations.Landscape | DisplayOrientations.LandscapeFlipped;
            var hidden = StatusBar.GetForCurrentView().HideAsync();
            Commands.Visibility = Visibility.Collapsed;
            InfoArea.Visibility = Visibility.Collapsed;
            Layout();
        }

        void ExitFullscreen()
        {
            PlayerService.Log("fullscreen off");
            fullscreen = false;
            DisplayInformation.AutoRotationPreferences = DisplayOrientations.Portrait;
            var shown = StatusBar.GetForCurrentView().ShowAsync();
            Commands.IsOpen = false;
            Commands.Visibility = Visibility.Visible;
            InfoArea.Visibility = Visibility.Visible;
            Layout();
        }
    }
}
