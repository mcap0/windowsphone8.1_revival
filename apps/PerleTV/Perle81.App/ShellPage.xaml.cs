/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * App shell: keeps the shared MediaElement alive while app pages navigate.
 */

using System;
using Windows.Foundation;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class ShellPage : Page
    {
        public static ShellPage Current { get; private set; }

        readonly PlayerService playerService;
        FrameworkElement playerPageArea;
        bool playerPageVisible;
        bool fullscreen;
        // while casting the mini bar is the TV's: title, moving time, play/pause
        readonly DispatcherTimer castTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        // our video controls: shown by a tap, hidden 4 s later while playing
        readonly DispatcherTimer controlsTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        readonly DispatcherTimer seekTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
        readonly DispatcherTimer flashTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
        DateTime controlsHideAt;
        bool settingSeek;
        int flashSum;

        public ShellPage()
        {
            InitializeComponent();
            Current = this;

            playerService = PlayerService.Initialize();
            // under our controls and the subtitles
            PlayerHost.Children.Insert(0, playerService.Player);
            playerService.Changed += OnPlayerChanged;
            controlsTimer.Tick += (s, e) => UpdateVideoControls();
            seekTimer.Tick += OnSeekTimerTick;
            flashTimer.Tick += (s, e) => { flashTimer.Stop(); SeekFlash.Visibility = Visibility.Collapsed; };

            Loaded += OnLoaded;
            SizeChanged += OnShellSizeChanged;
            AppFrame.SizeChanged += OnFrameSizeChanged;
            // no page transitions: the video overlay is placed from the page layout, a sliding page would offset it
            AppFrame.ContentTransitions = new Windows.UI.Xaml.Media.Animation.TransitionCollection();
            castTimer.Tick += (s, e) => UpdateMiniBar();
            PerleCast.CastUi.ActiveChanged += OnCastActiveChanged;
            if (PerleCast.CastUi.IsActive) castTimer.Start();
            UpdateMiniBar();
        }

        void OnCastActiveChanged(object sender, object active)
        {
            if (PerleCast.CastUi.IsActive) castTimer.Start(); else castTimer.Stop();
            UpdateMiniBar();
            UpdatePlayerBounds();
        }

        static string Clock(double seconds)
        {
            var time = TimeSpan.FromSeconds(Math.Max(0, Math.Floor(seconds)));
            return time.TotalHours >= 1 ? time.ToString(@"h\:mm\:ss") : time.ToString(@"m\:ss");
        }

        void OnLoaded(object sender, RoutedEventArgs e)
        {
            UpdatePlayerBounds();
            UpdateMiniBar();
        }

        void OnShellSizeChanged(object sender, SizeChangedEventArgs e)
        {
            UpdatePlayerBounds();
        }

        void OnFrameSizeChanged(object sender, SizeChangedEventArgs e)
        {
            UpdatePlayerBounds();
        }

        public void SetPlayerPageVisible(bool visible, FrameworkElement videoArea, bool isFullscreen)
        {
            playerPageVisible = visible;
            playerPageArea = visible ? videoArea : null;
            fullscreen = visible && isFullscreen;
            if (!visible) HostSubtitle.Text = "";
            UpdateMiniBar();
            UpdatePlayerBounds();
            // once more after the page has its final layout
            var again = Dispatcher.RunIdleAsync(_ => UpdatePlayerBounds());
            if (videoArea != null)
            {
                videoArea.SizeChanged -= OnVideoAreaSizeChanged;
                if (visible) videoArea.SizeChanged += OnVideoAreaSizeChanged;
            }
        }

        void OnVideoAreaSizeChanged(object sender, SizeChangedEventArgs e)
        {
            UpdatePlayerBounds();
        }

        public void UpdatePlayerPageBounds(FrameworkElement videoArea, bool isFullscreen)
        {
            if (!playerPageVisible) return;
            playerPageArea = videoArea;
            fullscreen = isFullscreen;
            UpdatePlayerBounds();
        }

        public void SetSubtitle(string text, bool visible)
        {
            HostSubtitle.Text = text ?? "";
            HostSubtitle.Visibility = visible && !string.IsNullOrEmpty(text)
                ? Visibility.Visible : Visibility.Collapsed;
        }

        void OnPlayerChanged(object sender, EventArgs e)
        {
            if (!Dispatcher.HasThreadAccess)
            {
                var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    UpdateMiniBar();
                    UpdatePlayerBounds();
                });
                return;
            }
            UpdateMiniBar();
            UpdatePlayerBounds();
        }

        void UpdateMiniBar()
        {
            bool casting = PerleCast.CastUi.IsActive;
            bool showMini = !playerPageVisible && (playerService.HasVideo || casting);
            MiniBar.Visibility = showMini ? Visibility.Visible : Visibility.Collapsed;
            if (showMini) { var again = Dispatcher.RunIdleAsync(_ => UpdatePlayerBounds()); }
            AppFrame.Margin = showMini ? new Thickness(0, 0, 0, 72) : new Thickness(0);

            MiniTvGlyph.Visibility = casting ? Visibility.Visible : Visibility.Collapsed;
            MiniClose.Visibility = casting ? Visibility.Collapsed : Visibility.Visible;
            if (casting)
            {
                MiniTitle.Text = PerleCast.CastUi.TvTitle;
                double duration = PerleCast.CastUi.TvDurationSeconds;
                MiniUploader.Text = "TV · " + Clock(PerleCast.CastUi.TvPositionSeconds) +
                    (duration > 0 ? " / " + Clock(duration) : "");
                PlayPauseGlyph.Text = PerleCast.CastUi.TvPlaying ? "\u2161" : "\u25B6";
                return;
            }

            if (playerService.CurrentInfo != null)
            {
                MiniTitle.Text = playerService.CurrentInfo.Name ?? "";
                MiniUploader.Text = playerService.CurrentInfo.UploaderName ?? "";
            }
            else
            {
                MiniTitle.Text = "";
                MiniUploader.Text = "";
            }

            PlayPauseGlyph.Text = playerService.IsPlaying ? "\u2161" : "\u25B6";
            if (!playerPageVisible && playerService.HasVideo)
                HostSubtitle.Visibility = Visibility.Collapsed;
        }

        void UpdatePlayerBounds()
        {
            // the system play/seek controls on the video page, none in the mini bar (a tap there opens the video)
            bool big = playerPageVisible && playerPageArea != null;
            // never the built-in controls or the MediaElement's own full window: once it is on, the video covers
            // every page and nothing in the app can turn it off
            playerService.Player.AreTransportControlsEnabled = false;
            try { if (playerService.Player.IsFullWindow) { PlayerService.Log("full window was on"); playerService.Player.IsFullWindow = false; } }
            catch (Exception) { }
            PlayerHost.IsHitTestVisible = big;
            if (!big) HideVideoControls();
            ControlsFullscreenGlyph.Text = fullscreen ? "\uE1D8" : "\uE1D9";
            if (playerPageVisible && playerPageArea != null)
            {
                try
                {
                    Point point = playerPageArea.TransformToVisual(Root).TransformPoint(new Point(0, 0));
                    PlayerHost.Width = Math.Max(0, playerPageArea.ActualWidth);
                    PlayerHost.Height = Math.Max(0, playerPageArea.ActualHeight);
                    PlayerHost.Margin = new Thickness(Math.Max(0, point.X), Math.Max(0, point.Y), 0, 0);
                    PlayerHost.Visibility = PlayerService.CurrentOrNull != null &&
                        PlayerService.CurrentOrNull.HasVideo ? Visibility.Visible : Visibility.Collapsed;
                    HostSubtitle.FontSize = fullscreen ? 24 : 20;
                    return;
                }
                catch (Exception)
                {
                    PlayerHost.Visibility = Visibility.Collapsed;
                }
            }

            // while casting the slot shows "TV", not the paused phone video
            if (!playerPageVisible && playerService.HasVideo && MiniBar.Visibility == Visibility.Visible &&
                !PerleCast.CastUi.IsActive)
            {
                try
                {
                    Point point = MiniVideoSlot.TransformToVisual(Root).TransformPoint(new Point(0, 0));
                    PlayerHost.Width = MiniVideoSlot.ActualWidth;
                    PlayerHost.Height = MiniVideoSlot.ActualHeight;
                    PlayerHost.Margin = new Thickness(Math.Max(0, point.X), Math.Max(0, point.Y), 0, 0);
                    PlayerHost.Visibility = Visibility.Visible;
                    return;
                }
                catch (Exception)
                {
                    PlayerHost.Visibility = Visibility.Collapsed;
                }
            }

            PlayerHost.Visibility = Visibility.Collapsed;
        }

        // ---- video controls (video page only) ----

        void OnVideoTapped(object sender, TappedRoutedEventArgs e)
        {
            if (VideoControls.Visibility == Visibility.Visible) HideVideoControls();
            else ShowVideoControls();
        }

        /// <summary>Double tap: left third -10 s, right third +10 s, middle = full screen (like YouTube).</summary>
        void OnVideoDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            e.Handled = true;
            var page = AppFrame.Content as PlayerPage;
            if (page == null || !playerPageVisible) return;
            double x = e.GetPosition(VideoGestures).X, width = VideoGestures.ActualWidth;
            int step = page.DoubleTapAt(x, width);
            if (step == 0) { HideVideoControls(); return; }
            // the first tap of the pair toggled the controls: leave them as they were before
            if (VideoControls.Visibility == Visibility.Visible) HideVideoControls(); else ShowVideoControls();
            flashSum = flashTimer.IsEnabled && Math.Sign(flashSum) == Math.Sign(step) ? flashSum + step : step;
            SeekFlash.Text = (flashSum > 0 ? "+" : "\u2212") + Math.Abs(flashSum) + " s";
            SeekFlash.HorizontalAlignment = step > 0 ? HorizontalAlignment.Right : HorizontalAlignment.Left;
            SeekFlash.Visibility = Visibility.Visible;
            flashTimer.Stop();
            flashTimer.Start();
            UpdateVideoControls();
        }

        public void ShowVideoControls()
        {
            if (!playerPageVisible || !playerService.HasVideo) return;
            VideoControls.Visibility = Visibility.Visible;
            HostSubtitleBox.Margin = new Thickness(12, 0, 12, 60);
            controlsHideAt = DateTime.UtcNow.AddSeconds(4);
            UpdateVideoControls();
            controlsTimer.Start();
        }

        void HideVideoControls()
        {
            controlsTimer.Stop();
            VideoControls.Visibility = Visibility.Collapsed;
            HostSubtitleBox.Margin = new Thickness(12, 0, 12, 12);
        }

        void UpdateVideoControls()
        {
            if (VideoControls.Visibility != Visibility.Visible) return;
            var player = playerService.Player;
            bool playing = playerService.IsPlaying;
            if (playing && DateTime.UtcNow > controlsHideAt && !seekTimer.IsEnabled) { HideVideoControls(); return; }
            ControlsPlayGlyph.Text = playing ? "\uE103" : "\uE102";
            double duration = 0;
            try { if (player.NaturalDuration.HasTimeSpan) duration = player.NaturalDuration.TimeSpan.TotalSeconds; }
            catch (Exception) { }
            ControlsDuration.Text = duration > 0 ? Clock(duration) : "";
            if (seekTimer.IsEnabled) return;  // the user is moving the slider
            double position = player.Position.TotalSeconds;
            ControlsPosition.Text = Clock(position);
            settingSeek = true;
            ControlsSeek.Maximum = Math.Max(1, duration);
            ControlsSeek.Value = Math.Min(Math.Max(0, position), ControlsSeek.Maximum);
            settingSeek = false;
        }

        void OnControlsPlayTapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
            if (PerleCast.CastUi.IsActive) return;
            playerService.TogglePlayPause();
            controlsHideAt = DateTime.UtcNow.AddSeconds(4);
            UpdateVideoControls();
        }

        void OnControlsFullscreenTapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
            var page = AppFrame.Content as PlayerPage;
            if (page != null) page.ToggleFullscreen();
            controlsHideAt = DateTime.UtcNow.AddSeconds(4);
        }

        void OnControlsInnerTapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;  // a tap on the slider must not hide the controls
        }

        void OnControlsSeekChanged(object sender, Windows.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (settingSeek) return;
            ControlsPosition.Text = Clock(e.NewValue);
            controlsHideAt = DateTime.UtcNow.AddSeconds(4);
            // seek once the finger rests: each seek reopens HLS segments
            seekTimer.Stop();
            seekTimer.Start();
        }

        void OnSeekTimerTick(object sender, object e)
        {
            seekTimer.Stop();
            if (PerleCast.CastUi.IsActive || !playerService.HasVideo) return;
            try { playerService.Player.Position = TimeSpan.FromSeconds(ControlsSeek.Value); }
            catch (Exception) { }
            UpdateVideoControls();
        }

        void OnMiniBarTapped(object sender, TappedRoutedEventArgs e)
        {
            if (PerleCast.CastUi.IsActive) PerleCast.CastUi.Reopen();
            else if (playerService.HasVideo) NavigateToCurrentVideo();
        }

        void OnPlayPauseTapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
            OnPlayPauseClick(sender, e);
        }

        void OnCloseTapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
            OnCloseClick(sender, e);
        }

        void NavigateToCurrentVideo()
        {
            if (!playerService.HasVideo || string.IsNullOrEmpty(playerService.CurrentUrl)) return;
            if (AppFrame.Content is PlayerPage) return;
            AppFrame.Navigate(typeof(PlayerPage), playerService.CurrentUrl);
        }

        void OnPlayPauseClick(object sender, RoutedEventArgs e)
        {
            if (PerleCast.CastUi.IsActive) { PerleCast.CastUi.TogglePlayPause(); UpdateMiniBar(); return; }
            playerService.TogglePlayPause();
        }

        void OnCloseClick(object sender, RoutedEventArgs e)
        {
            playerService.Close();
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            if (Current == this) Current = null;
            base.OnNavigatedFrom(e);
        }
    }
}
