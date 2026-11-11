using Windows.ApplicationModel.Activation;
using Windows.Services.Maps;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Mappe
{
    sealed partial class App : Application
    {
        public const string CrashKey = "lastCrash";

        public App()
        {
            this.InitializeComponent();
            this.UnhandledException += App_UnhandledException;
        }

        // Keep the app alive and show the error on screen: there is no other way to read crashes on the phone.
        private void App_UnhandledException(object sender, Windows.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            e.Handled = true;
            string text = e.Exception != null ? e.Exception.ToString() : e.Message;
            Windows.Storage.ApplicationData.Current.LocalSettings.Values[CrashKey] = text.Length > 2000 ? text.Substring(0, 2000) : text;
            var page = (Window.Current.Content as Frame)?.Content as MainPage;
            if (page != null) page.ShowCrash();
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            // No Here/Bing token: the OSM tiles do not need one. An empty token may be rejected, so ignore errors.
            try
            {
                MapService.ServiceToken = "-";
            }
            catch (System.Exception)
            {
            }

            Frame rootFrame = Window.Current.Content as Frame;
            if (rootFrame == null)
            {
                rootFrame = new Frame();
                Window.Current.Content = rootFrame;
            }
            if (rootFrame.Content == null)
            {
                rootFrame.Navigate(typeof(MainPage), e.Arguments);
            }
            Window.Current.Activate();
        }
    }
}
