using Windows.ApplicationModel.Activation;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Authenticator
{
    sealed partial class App : Application
    {
        public App() { InitializeComponent(); }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            if (!(Window.Current.Content is Frame frame))
            {
                frame = new Frame();
                frame.Navigated += (s, a) => SystemNavigationManager.GetForCurrentView().AppViewBackButtonVisibility =
                    frame.CanGoBack ? AppViewBackButtonVisibility.Visible : AppViewBackButtonVisibility.Collapsed;
                SystemNavigationManager.GetForCurrentView().BackRequested += (s, a) =>
                {
                    if (frame.CanGoBack) { a.Handled = true; frame.GoBack(); }
                };
                Window.Current.Content = frame;
            }
            if (frame.Content == null) frame.Navigate(typeof(MainPage));
            Window.Current.Activate();
        }
    }
}
