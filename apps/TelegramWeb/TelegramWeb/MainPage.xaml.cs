using System;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.Data.Json;
using Windows.Phone.UI.Input;
using Windows.Storage;
using Windows.System;
using Windows.UI.Popups;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;
using Windows.Graphics.Display;
using Windows.Foundation;
using Windows.ApplicationModel.Activation;
using TelegramWeb.WebKit;

namespace TelegramWeb
{
    public sealed partial class MainPage : Page
    {
        // Telegram Web puts the unread count in the title: "(3) Telegram Web", "(99+) ...".
        private static readonly Regex UnreadPattern = new Regex(@"\((\d+)\+?\)");

        private readonly WebEngine engine;
        private int badgeCount = -1;

        public MainPage()
        {
            this.InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;

            engine = new WebEngine(Dispatcher, this, WebHost, GpuPanel, WebImage, KeyboardProxy);
            engine.TitleChanged += OnTitleChanged;
            engine.ConsoleMessage += OnConsoleMessage;
            engine.LoadFinished += () => StatusText.Visibility = Visibility.Collapsed;
            engine.AlertMessage += text => ShowMessage(text);
            engine.AttachInput();

            Loaded += OnLoaded;

            HardwareButtons.BackPressed += OnBackPressed;

            InputPane pane = InputPane.GetForCurrentView();
            pane.Showing += OnInputPaneShowing;
            pane.Hiding += OnInputPaneHiding;

            Application.Current.Suspending += OnSuspending;
            Application.Current.Resuming += OnResuming;
            MemoryManager.AppMemoryUsageIncreased += OnMemoryUsageIncreased;
        }

        private async void OnLoaded(object sender, RoutedEventArgs e)
        {
            Loaded -= OnLoaded;
            try
            {
                // Own data folder, separate from the MetroSurf browser's storage.
                StorageFolder webData = await ApplicationData.Current.LocalFolder.CreateFolderAsync("webdata", CreationCollisionOption.OpenIfExists);
                bool ok = await engine.StartAsync(Package.Current.InstalledLocation.Path, webData.Path, App.RestoringSession);
                if (!ok)
                {
                    StatusText.Text = "The web engine failed to start.";
                    return;
                }
                engine.HostSizeChanged();
            }
            catch (Exception ex)
            {
                StatusText.Text = "Could not start: " + ex.Message;
            }
        }

        // Keyboard: keep the page above the on-screen keyboard. The layout shrinks, so the
        // WebHost size change resizes the engine view. EnsuredFocusedElementInView stops the
        // OS from scrolling the whole page.
        private void OnInputPaneShowing(InputPane sender, InputPaneVisibilityEventArgs args)
        {
            args.EnsuredFocusedElementInView = true;
            Root.Margin = new Thickness(0, 0, 0, args.OccludedRect.Height);
        }

        private void OnInputPaneHiding(InputPane sender, InputPaneVisibilityEventArgs args)
        {
            args.EnsuredFocusedElementInView = true;
            Root.Margin = new Thickness(0);
        }

        private void OnBackPressed(object sender, BackPressedEventArgs e)
        {
            if (engine.TryGoBack())
            {
                e.Handled = true;
            }
            // Otherwise not handled: the system leaves the app, as with any root page.
        }

        private void OnTitleChanged(string title)
        {
            int count = 0;
            Match m = UnreadPattern.Match(title ?? "");
            if (m.Success)
            {
                int.TryParse(m.Groups[1].Value, out count);
            }
            count = Math.Min(count, 99);
            if (count == badgeCount) return;
            badgeCount = count;
            Notifications.SetBadge(count);
        }

        private void OnConsoleMessage(string message)
        {
            if (message == null || !message.StartsWith(UserScripts.NotifyPrefix, StringComparison.Ordinal)) return;
            try
            {
                JsonObject payload = JsonObject.Parse(message.Substring(UserScripts.NotifyPrefix.Length));
                string title = Named(payload, "t");
                string body = Named(payload, "b");
                Notifications.ShowToast(string.IsNullOrEmpty(title) ? "Telegram" : title, body);
            }
            catch (Exception)
            {
                // Malformed payload: ignore.
            }
        }

        private static string Named(JsonObject obj, string key)
        {
            return obj.ContainsKey(key) && obj.GetNamedValue(key).ValueType == JsonValueType.String
                ? obj.GetNamedString(key)
                : "";
        }

        private async void ShowMessage(string text)
        {
            await new MessageDialog(text ?? "").ShowAsync();
        }

        private async void OnSuspending(object sender, SuspendingEventArgs e)
        {
            SuspendingDeferral deferral = e.SuspendingOperation.GetDeferral();
            await Task.Run(() => engine.Suspend());
            deferral.Complete();
        }

        private void OnResuming(object sender, object e)
        {
            engine.Resume();
        }

        private void OnMemoryUsageIncreased(object sender, object e)
        {
            // Same rule as MetroSurf: warn the engine at the over-limit level.
            if ((int)MemoryManager.AppMemoryUsageLevel == 2)
            {
                engine.MemoryUsageIncreased(MemoryManager.AppMemoryUsage > MemoryManager.AppMemoryUsageLimit / 10 * 9);
            }
        }
    }
}
