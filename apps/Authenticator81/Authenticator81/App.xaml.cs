using System;
using System.Threading.Tasks;
using Authenticator.Core;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.Storage;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Authenticator
{
    sealed partial class App : Application
    {
        public const string CrashKey = "lastCrash";
        public const string PickerTargetKey = "photoPickerTarget";

        public static long UnixNow()
        {
            return (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
        }

        public static readonly AccountStore Store = new AccountStore();

        public App()
        {
            InitializeComponent();
            UnhandledException += OnUnhandledException;
            Windows.Phone.UI.Input.HardwareButtons.BackPressed += OnBackPressed;
            Suspending += OnSuspending;
            Resuming += OnResuming;
        }

        public static void SetPickerTarget(string target)
        {
            ApplicationData.Current.LocalSettings.Values[PickerTargetKey] = target;
        }

        public static string GetPickerTarget()
        {
            try
            {
                return ApplicationData.Current.LocalSettings.Values[PickerTargetKey] as string;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static void ClearPickerTarget()
        {
            try { ApplicationData.Current.LocalSettings.Values.Remove(PickerTargetKey); }
            catch (Exception) { }
        }

        void OnBackPressed(object sender, Windows.Phone.UI.Input.BackPressedEventArgs e)
        {
            var frame = Window.Current.Content as Frame;
            if (frame != null && frame.CanGoBack)
            {
                e.Handled = true;
                frame.GoBack();
            }
        }

        void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            try { ApplicationData.Current.LocalSettings.Values[CrashKey] = e.Exception.ToString(); }
            catch (Exception) { }
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
                rootFrame.Navigate(typeof(MainPage), e.Arguments);

            Window.Current.Activate();
        }

        protected override async void OnActivated(IActivatedEventArgs args)
        {
            Frame rootFrame = Window.Current.Content as Frame;
            if (rootFrame == null)
            {
                rootFrame = new Frame();
                Window.Current.Content = rootFrame;
            }

            var pickerArgs = args as FileOpenPickerContinuationEventArgs;
            if (pickerArgs != null)
            {
                string target = GetPickerTarget();
                Type pageType = target == "scan" ? typeof(ScanPage) : typeof(AddPage);

                if (rootFrame.Content == null || rootFrame.Content.GetType() != pageType)
                    rootFrame.Navigate(pageType);

                Window.Current.Activate();

                var continuable = rootFrame.Content as IFileOpenPickerContinuable;
                if (continuable != null)
                    continuable.ContinueFileOpenPicker(pickerArgs);
                else
                    ClearPickerTarget();

                return;
            }

            var protocolArgs = args as ProtocolActivatedEventArgs;
            if (protocolArgs != null)
            {
                if (rootFrame.Content == null)
                    rootFrame.Navigate(typeof(MainPage));

                Window.Current.Activate();

                try
                {
                    string uri = protocolArgs.Uri == null ? null : protocolArgs.Uri.AbsoluteUri;
                    OtpImportResult result = OtpUri.ParseImport(uri);
                    await AccountImport.ImportAsync(result);
                }
                catch (Exception ex)
                {
                    var dialog = new MessageDialog("QR non valido: " + ex.Message, "Importazione");
                    await dialog.ShowAsync();
                }

                return;
            }

            if (rootFrame.Content == null)
                rootFrame.Navigate(typeof(MainPage));

            Window.Current.Activate();
        }

        async void OnSuspending(object sender, SuspendingEventArgs e)
        {
            var deferral = e.SuspendingOperation.GetDeferral();
            try
            {
                var frame = Window.Current.Content as Frame;
                var page = frame == null ? null : frame.Content as ScanPage;
                if (page != null)
                    await page.SuspendCameraAsync();
            }
            finally
            {
                deferral.Complete();
            }
        }

        void OnResuming(object sender, object e)
        {
            var frame = Window.Current.Content as Frame;
            var page = frame == null ? null : frame.Content as ScanPage;
            if (page != null)
                page.ResumeCameraAsync();
        }
    }
}
