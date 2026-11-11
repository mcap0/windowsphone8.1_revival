using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace TelegramWeb
{
    sealed partial class App : Application
    {
        // True when Windows restored the app after it was terminated (session restore hint).
        public static bool RestoringSession;

        public App()
        {
            this.InitializeComponent();
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            RestoringSession = e.PreviousExecutionState == ApplicationExecutionState.Terminated;

            Frame rootFrame = Window.Current.Content as Frame;
            if (rootFrame == null)
            {
                rootFrame = new Frame();
                Window.Current.Content = rootFrame;
            }
            if (rootFrame.Content == null)
            {
                rootFrame.Navigate(typeof(MainPage));
            }
            Window.Current.Activate();
        }
    }
}
