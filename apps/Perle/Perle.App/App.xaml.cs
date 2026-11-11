using Windows.ApplicationModel.Activation;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Perle.App
{
    sealed partial class App : Application
    {
        public App()
        {
            InitializeComponent();
            CastHost.Install();
            Perle.Cast.CastService.PauseForCasting = PlayerPage.PauseForCasting;
        }

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
