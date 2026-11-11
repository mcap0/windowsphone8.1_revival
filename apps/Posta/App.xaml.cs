using System;
using Posta.Pages;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Posta
{
    sealed partial class App : Application
    {
        public App()
        {
            InitializeComponent();
            Suspending += OnSuspending;
            Windows.Phone.UI.Input.HardwareButtons.BackPressed += OnBackPressed;
        }

        private void OnBackPressed(object sender, Windows.Phone.UI.Input.BackPressedEventArgs e)
        {
            var frame = Window.Current.Content as Frame;
            var files = frame == null ? null : frame.Content as FilePage;
            if (files != null && files.HandleBack())
            {
                e.Handled = true;
                return;
            }
            var message = frame == null ? null : frame.Content as MessagePage;
            if (message != null && message.HandleBack())
            {
                e.Handled = true;
                return;
            }
            if (frame != null && frame.CanGoBack)
            {
                e.Handled = true;
                frame.GoBack();
            }
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            EnsureRootFrame();
            Window.Current.Activate();
            try { PostaTask.Checks.Register(); } catch (Exception) { }
        }

        protected override async void OnActivated(IActivatedEventArgs args)
        {
            if (args.Kind == ActivationKind.PickSaveFileContinuation)
            {
                var saveArgs = args as FileSavePickerContinuationEventArgs;
                if (saveArgs != null)
                    await MessagePage.CompleteFileSaveAsync(saveArgs);
            }

            EnsureRootFrame();
            Window.Current.Activate();
        }

        private Frame EnsureRootFrame()
        {
            var rootFrame = Window.Current.Content as Frame;
            if (rootFrame == null)
            {
                rootFrame = new Frame();
                rootFrame.NavigationFailed += OnNavigationFailed;
                Window.Current.Content = rootFrame;
            }

            if (rootFrame.Content == null)
                rootFrame.Navigate(typeof(InboxPage));

            return rootFrame;
        }

        private void OnNavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            throw new Exception("Impossibile aprire la pagina " + e.SourcePageType.FullName);
        }

        private void OnSuspending(object sender, SuspendingEventArgs e)
        {
            var deferral = e.SuspendingOperation.GetDeferral();
            deferral.Complete();
        }
    }
}
