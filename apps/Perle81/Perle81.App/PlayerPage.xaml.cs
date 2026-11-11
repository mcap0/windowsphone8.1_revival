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
            AudioCommand.Visibility = service.AudioOptionCount > 1
                ? Visibility.Visible : Visibility.Collapsed;
            UpdateSubtitleDisplay();
        }

        void OnAudioCommandClick(object sender, RoutedEventArgs e)
        {
            if (service == null || service.AudioOptionCount <= 1) return;

            var flyout = new MenuFlyout();
            foreach (AudioOption option in service.AudioOptions)
            {
                bool current = IsCurrentAudio(option);
                var item = new MenuFlyoutItem
                {
                    Text = (current ? "✓ " : "") + AudioLabel(option),
                    Tag = option
                };
                item.Click += OnAudioOptionClick;
                flyout.Items.Add(item);
            }
            flyout.ShowAt(AudioCommand);
        }

        async void OnAudioOptionClick(object sender, RoutedEventArgs e)
        {
            var item = sender as MenuFlyoutItem;
            var option = item == null ? null : item.Tag as AudioOption;
            if (option == null || service == null) return;
            await service.ChangeAudioAsync(option);
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

            var flyout = new MenuFlyout();
            foreach (SubtitleChoice choice in choices)
            {
                bool current = IsCurrentSubtitle(choice);
                var item = new MenuFlyoutItem
                {
                    Text = (current ? "✓ " : "") + choice.Label,
                    Tag = choice
                };
                item.Click += OnSubtitleOptionClick;
                flyout.Items.Add(item);
            }
            flyout.ShowAt(SubtitleCommand);
        }

        async void OnSubtitleOptionClick(object sender, RoutedEventArgs e)
        {
            var item = sender as MenuFlyoutItem;
            var choice = item == null ? null : item.Tag as SubtitleChoice;
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

        void OnFullscreenClick(object sender, RoutedEventArgs e)
        {
            if (fullscreen) ExitFullscreen(); else EnterFullscreen();
        }

        void OnVideoTapped(object sender, Windows.UI.Xaml.Input.TappedRoutedEventArgs e)
        {
            if (!fullscreen) return;
            Commands.Visibility = Visibility.Visible;
            Commands.IsOpen = true;
        }

        public bool HandleBack()
        {
            if (!fullscreen) return false;
            ExitFullscreen();
            return true;
        }

        void EnterFullscreen()
        {
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
