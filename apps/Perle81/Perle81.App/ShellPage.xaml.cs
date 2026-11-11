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

        public ShellPage()
        {
            InitializeComponent();
            Current = this;

            playerService = PlayerService.Initialize();
            PlayerHost.Children.Add(playerService.Player);
            playerService.Changed += OnPlayerChanged;

            Loaded += OnLoaded;
            SizeChanged += OnShellSizeChanged;
            AppFrame.SizeChanged += OnFrameSizeChanged;
            // no page transitions: the video overlay is placed from the page layout, a sliding page would offset it
            AppFrame.ContentTransitions = new Windows.UI.Xaml.Media.Animation.TransitionCollection();
            UpdateMiniBar();
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
            bool showMini = !playerPageVisible && playerService.HasVideo;
            MiniBar.Visibility = showMini ? Visibility.Visible : Visibility.Collapsed;
            if (showMini) { var again = Dispatcher.RunIdleAsync(_ => UpdatePlayerBounds()); }
            AppFrame.Margin = showMini ? new Thickness(0, 0, 0, 72) : new Thickness(0);

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
            playerService.Player.AreTransportControlsEnabled = big;
            PlayerHost.IsHitTestVisible = big;
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

            if (!playerPageVisible && playerService.HasVideo && MiniBar.Visibility == Visibility.Visible)
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

        void OnMiniBarTapped(object sender, TappedRoutedEventArgs e)
        {
            if (playerService.HasVideo) NavigateToCurrentVideo();
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
