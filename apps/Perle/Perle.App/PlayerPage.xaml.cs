using System;
using Perle.Cast;
using Perle.Extractor;
using Windows.Graphics.Display;
using Windows.Media.Playback;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Perle.App
{
    public sealed partial class PlayerPage : Page
    {
        readonly MediaPlayer mediaPlayer = new MediaPlayer();
        StreamInfo info;

        /// <summary>The page playing a video, for CastService.PauseForCasting.</summary>
        static PlayerPage current;

        public PlayerPage()
        {
            InitializeComponent();
            Player.SetMediaPlayer(mediaPlayer);
            SizeChanged += (s, e) => Layout();
        }

        /// <summary>16:9 video in portrait, full screen in landscape.</summary>
        void Layout()
        {
            bool landscape = ActualWidth > ActualHeight;
            VideoArea.Height = landscape ? ActualHeight : ActualWidth * 9 / 16;
            Grid.SetRowSpan(VideoArea, landscape ? 2 : 1);
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            DisplayInformation.AutoRotationPreferences = DisplayOrientations.Portrait | DisplayOrientations.Landscape | DisplayOrientations.LandscapeFlipped;
            var url = (string)e.Parameter;
            try
            {
                info = await YoutubeStream.GetInfoAsync(url);
                TitleText.Text = info.Name ?? "";
                MetaText.Text = info.UploaderName + (info.ViewCount >= 0 ? " · " + Format.Views(info.ViewCount) : "");
                DescriptionText.Text = info.Description ?? "";
                CastButton.IsEnabled = CastLauncher.IsSupported(info);
                if (CastLauncher.PlayOnTvIfCasting(info))
                {
                    // while casting, a video opened in the app goes to the TV
                    CastLauncher.OpenController(Frame);
                    return;
                }
                current = this;
                mediaPlayer.Source = await Playback.CreateSourceAsync(info);
                long resume = Positions.Get(info.Url, info.Duration);
                if (resume > 0) mediaPlayer.PlaybackSession.Position = TimeSpan.FromSeconds(resume);
                await AppState.Repository.AddToHistoryAsync(info.Url);
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Impossibile riprodurre: " + ex.Message;
            }
            finally { Busy.IsActive = false; }
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            DisplayInformation.AutoRotationPreferences = DisplayOrientations.Portrait;
            if (current == this) current = null;
            SavePosition();
            mediaPlayer.Pause();
            mediaPlayer.Source = null;
        }

        void SavePosition()
        {
            if (info == null || mediaPlayer.Source == null) return;
            Positions.Save(info.Url, (long)mediaPlayer.PlaybackSession.Position.TotalSeconds);
        }

        async void OnCast(object sender, RoutedEventArgs e)
        {
            if (info != null) await CastLauncher.ShowAsync(Frame, info);
        }

        /// <summary>
        /// Pauses the phone player when its video goes to the TV (PlayerHolder.pauseForCasting).
        /// Called on any thread.
        /// </summary>
        /// <returns>where the player was in milliseconds, -1 if it was not playing url</returns>
        public static long PauseForCasting(string url)
        {
            var page = current;
            if (page == null || page.info?.Url != url) return -1;
            long position = (long)page.mediaPlayer.PlaybackSession.Position.TotalMilliseconds;
            page.mediaPlayer.Pause();
            page.SavePosition();
            return position;
        }
    }
}
