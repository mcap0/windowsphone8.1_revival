using System;
using Calendario.Logic;
using Windows.ApplicationModel.Activation;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Calendario
{
    sealed partial class App : Application
    {
        /// <summary>Event handed from the agenda/month list to the detail page.</summary>
        public static EventItem SelectedEvent;

        public const string LastCrashKey = "lastCrash";

        public App()
        {
            this.InitializeComponent();
            this.UnhandledException += App_UnhandledException;
            Windows.Phone.UI.Input.HardwareButtons.BackPressed += OnBackPressed;
        }

        // Tasto Indietro: dal dettaglio torna alla lista; sulla pagina principale lascia uscire.
        private void OnBackPressed(object sender, Windows.Phone.UI.Input.BackPressedEventArgs e)
        {
            var frame = Window.Current.Content as Frame;
            if (frame != null && frame.CanGoBack)
            {
                e.Handled = true;
                frame.GoBack();
            }
        }

        private void App_UnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            try
            {
                ApplicationData.Current.LocalSettings.Values[LastCrashKey] = e.Exception.ToString();
            }
            catch (Exception)
            {
            }
            e.Handled = true;
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
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
