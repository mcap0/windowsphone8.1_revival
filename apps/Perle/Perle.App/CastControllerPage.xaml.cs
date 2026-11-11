/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.CastControllerActivity.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using Perle.Cast;
using Windows.ApplicationModel.DataTransfer;
using Windows.UI.Core;
using Windows.UI.Popups;
using Windows.UI.Text;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Perle.App
{
    /// <summary>
    /// Remote control for the video playing on the TV. The session itself lives in CastService,
    /// so this page can be left and opened again from the bar in the main page, the media
    /// controls or the "Cast to TV" button at any time.
    /// </summary>
    public sealed partial class CastControllerPage : Page
    {
        const double DisabledOpacity = 0.3;

        bool isUserSeeking;
        bool rendering;
        List<CastQueueEntry> shownQueue;
        int shownQueueIndex = -1;
        CastState state = new CastState();

        public CastControllerPage()
        {
            InitializeComponent();
            // the Slider handles the pointer events itself: listen to the handled ones too
            SeekBar.AddHandler(PointerPressedEvent, new PointerEventHandler((s, e) => isUserSeeking = true), true);
            SeekBar.AddHandler(PointerReleasedEvent, new PointerEventHandler((s, e) => OnSeekEnd()), true);
            SeekBar.AddHandler(PointerCaptureLostEvent, new PointerEventHandler((s, e) => OnSeekEnd()), true);
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            CastService.StateChanged += OnStateChanged;
            Render(CastService.State);
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            CastService.StateChanged -= OnStateChanged;
        }

        void OnStateChanged(CastState s) => _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => Render(s));

        void Render(CastState s)
        {
            state = s;
            if (!s.Active && s.Error == null)
            {
                // no session (anymore): nothing to control
                if (Frame.CanGoBack && Frame.Content == this) Frame.GoBack();
                return;
            }
            rendering = true;
            try
            {
                TitleText.Text = s.Title ?? "";
                UploaderText.Text = s.Uploader ?? "";
                UploaderText.Visibility = string.IsNullOrEmpty(s.Uploader) ? Visibility.Collapsed : Visibility.Visible;
                DeviceText.Text = string.Format(CastStrings.PlayingOn, s.DeviceName);
                StatusText.Text = CastStatuses.Text(s.Status);
                ErrorText.Visibility = s.Error != null ? Visibility.Visible : Visibility.Collapsed;
                ErrorText.Text = s.Error ?? "";

                bool controlsEnabled = s.Active;
                PlayPauseButton.IsEnabled = controlsEnabled;
                PlayPauseIcon.Symbol = s.Status == CastStatus.Playing ? Symbol.Pause : Symbol.Play;
                StopButton.IsEnabled = controlsEnabled;
                bool hasNext = s.QueueIndex + 1 < s.Queue.Count;
                NextButton.IsEnabled = controlsEnabled && hasNext;
                NextButton.Opacity = hasNext ? 1 : DisabledOpacity;

                long duration = s.DurationSeconds;
                SeekBar.IsEnabled = controlsEnabled && duration > 0;
                if (duration > 0)
                {
                    SeekBar.Maximum = duration;
                    DurationText.Text = Format.Duration(duration);
                }
                else DurationText.Text = "";
                if (!isUserSeeking)
                {
                    long position = Math.Max(0, s.PositionSeconds);
                    SeekBar.Value = Math.Min(position, SeekBar.Maximum);
                    PositionText.Text = Format.Duration(position);
                }

                VolumeControls.Visibility = s.HasVolumeControl ? Visibility.Visible : Visibility.Collapsed;
                VolumeUpButton.IsEnabled = controlsEnabled;
                VolumeDownButton.IsEnabled = controlsEnabled;
                VolumeText.Text = string.Format(CastStrings.Volume, s.Volume);

                QualityButton.Visibility = s.Qualities.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                QualityButton.IsEnabled = controlsEnabled;
                QualityButton.Content = CastStrings.Quality + ": " + QualityText(s);
                AudioTrackButton.Visibility = s.AudioTracks.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                AudioTrackButton.IsEnabled = controlsEnabled;
                AudioTrackButton.Content = s.AudioTrack >= 0 && s.AudioTrack < s.AudioTracks.Count ? s.AudioTracks[s.AudioTrack] : CastStrings.AudioTrack;
                SubtitlesButton.Visibility = s.Subtitles.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
                SubtitlesButton.IsEnabled = controlsEnabled;
                SubtitlesButton.Content = s.Subtitle >= 0 && s.Subtitle < s.Subtitles.Count ? CastStrings.Subtitles + ": " + s.Subtitles[s.Subtitle] : CastStrings.SubtitlesOff;

                RenderQueue(s);
            }
            finally { rendering = false; }
        }

        static string QualityText(CastState s)
        {
            var current = s.Quality > 0 ? s.Quality + "p" : null;
            if (!s.AutoQuality && current != null) return current;
            if (current != null) return string.Format(CastStrings.QualityAutoCurrent, current);
            return CastStrings.QualityAuto;
        }

        void RenderQueue(CastState s)
        {
            if (ReferenceEquals(s.Queue, shownQueue) && s.QueueIndex == shownQueueIndex) return;
            shownQueue = s.Queue;
            shownQueueIndex = s.QueueIndex;
            var visible = s.Queue.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
            QueueTitle.Visibility = visible;
            QueueList.Visibility = visible;
            QueueList.Children.Clear();
            if (s.Queue.Count <= 1) return;
            for (int i = 0; i < s.Queue.Count; i++)
            {
                int index = i;
                var entry = s.Queue[i];
                bool isCurrent = index == s.QueueIndex;
                var row = new Grid { Padding = new Thickness(0, 6, 0, 6), Background = new Windows.UI.Xaml.Media.SolidColorBrush(Windows.UI.Colors.Transparent) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(24) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var playing = new SymbolIcon(Symbol.Play) { Visibility = isCurrent ? Visibility.Visible : Visibility.Collapsed, VerticalAlignment = VerticalAlignment.Center };
                row.Children.Add(playing);
                var texts = new StackPanel();
                Grid.SetColumn(texts, 1);
                texts.Children.Add(new TextBlock { Text = entry.Title, TextWrapping = TextWrapping.Wrap, FontWeight = isCurrent ? FontWeights.Bold : FontWeights.Normal });
                var details = string.Join(" · ", new[] { entry.Uploader, entry.DurationSeconds > 0 ? Format.Duration(entry.DurationSeconds) : null }.Where(t => !string.IsNullOrEmpty(t)));
                texts.Children.Add(new TextBlock { Text = details, FontSize = 12, Opacity = 0.7 });
                row.Children.Add(texts);
                if (!isCurrent)
                {
                    var remove = new Button { Content = new SymbolIcon(Symbol.Cancel), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0) };
                    remove.Click += (sender, e) => CastService.Remove(index);
                    Grid.SetColumn(remove, 2);
                    row.Children.Add(remove);
                    row.Tapped += (sender, e) => CastService.PlayIndex(index);
                }
                QueueList.Children.Add(row);
            }
        }

        void OnSeekValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            if (rendering) return;
            PositionText.Text = Format.Duration((long)e.NewValue);
            // a tap on the bar, without dragging
            if (!isUserSeeking) CastService.Seek((long)e.NewValue);
        }

        void OnSeekEnd()
        {
            if (!isUserSeeking) return;
            isUserSeeking = false;
            CastService.Seek((long)SeekBar.Value);
        }

        void OnPlayPause(object sender, RoutedEventArgs e) => CastService.PlayPause();

        void OnStop(object sender, RoutedEventArgs e)
        {
            CastService.Stop();
            if (Frame.CanGoBack) Frame.GoBack();
        }

        void OnNext(object sender, RoutedEventArgs e) => CastService.Next();

        void OnVolumeUp(object sender, RoutedEventArgs e) => CastService.VolumeUp();

        void OnVolumeDown(object sender, RoutedEventArgs e) => CastService.VolumeDown();

        void OnCopyLog(object sender, RoutedEventArgs e) => CopyToClipboard(CastLog.DumpRecent());

        public static void CopyToClipboard(string text)
        {
            var package = new DataPackage();
            package.SetText(text);
            Clipboard.SetContent(package);
        }

        async void OnQuality(object sender, RoutedEventArgs e)
        {
            var qualities = state.Qualities;
            var names = new[] { CastStrings.QualityAuto }.Concat(qualities.Select(q => q + "p")).ToList();
            int checkedIndex = state.AutoQuality ? 0 : qualities.IndexOf(state.Quality) + 1;
            int which = await ChooseAsync(CastStrings.Quality, names, checkedIndex);
            if (which >= 0) CastService.SetQuality(which == 0 ? 0 : qualities[which - 1]);
        }

        async void OnAudioTrack(object sender, RoutedEventArgs e)
        {
            int which = await ChooseAsync(CastStrings.AudioTrack, state.AudioTracks, state.AudioTrack);
            if (which >= 0) CastService.SetAudioTrack(which);
        }

        async void OnSubtitles(object sender, RoutedEventArgs e)
        {
            var names = new[] { CastStrings.SubtitlesOff }.Concat(state.Subtitles).ToList();
            int which = await ChooseAsync(CastStrings.Subtitles, names, state.Subtitle + 1);
            if (which < 0) return;
            CastService.SetSubtitles(which - 1);
            if (which > 0) await new MessageDialog(CastStrings.SubtitlesHint).ShowAsync();
        }

        /// <returns>the index chosen, -1 if cancelled (AlertDialog.setSingleChoiceItems)</returns>
        static async System.Threading.Tasks.Task<int> ChooseAsync(string title, IList<string> names, int checkedIndex)
        {
            var dialog = new ContentDialog { Title = title, SecondaryButtonText = "Annulla" };
            var list = new ListView { SelectionMode = ListViewSelectionMode.Single, IsItemClickEnabled = true, ItemsSource = names.ToList(), MaxHeight = 400 };
            if (checkedIndex >= 0 && checkedIndex < names.Count) list.SelectedIndex = checkedIndex;
            int which = -1;
            list.ItemClick += (s, e) => { which = names.IndexOf((string)e.ClickedItem); dialog.Hide(); };
            dialog.Content = list;
            await dialog.ShowAsync();
            return which;
        }
    }
}
