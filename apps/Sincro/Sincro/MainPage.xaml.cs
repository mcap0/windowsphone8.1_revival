using System;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Sincro.Core;
using Sincro.Logic;
using Windows.ApplicationModel.Background;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Sincro
{
    public sealed partial class MainPage : Page
    {
        private bool _loading;
        private bool _running;

        public MainPage()
        {
            this.InitializeComponent();
        }

        private async void Page_Loaded(object sender, RoutedEventArgs e)
        {
            _loading = true;
            ContactsSwitch.IsOn = AppSettings.ContactsOn;
            CalendarSwitch.IsOn = AppSettings.CalendarOn;
            _loading = false;
            try { await BackgroundRegistration.EnsureAsync(); }
            catch (Exception ex) { ErrorText.Text = "Sfondo: " + ex.Message; }
            await RefreshAsync();
        }

        private async void Sync_Click(object sender, RoutedEventArgs e)
        {
            if (_running) return;
            _running = true;
            SetBusy(true);
            try
            {
                ErrorText.Text = "";
                SyncReport report = await SyncEngine.RunAsync(ContactsSwitch.IsOn, CalendarSwitch.IsOn, false);
                if (report.Errors.Count > 0)
                {
                    ErrorText.Text = string.Join("\n", report.Errors.ToArray());
                }
                await RefreshAsync(report);
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Errore: " + ex.Message;
            }
            finally
            {
                _running = false;
                SetBusy(false);
            }
        }

        private void Setup_Click(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(SetupPage));
        }

        private void Toggle_Toggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            AppSettings.ContactsOn = ContactsSwitch.IsOn;
            AppSettings.CalendarOn = CalendarSwitch.IsOn;
        }

        private void SetBusy(bool busy)
        {
            SyncButton.IsEnabled = !busy;
        }

        private async Task RefreshAsync(SyncReport last = null)
        {
            SyncState state = await SyncStateStore.LoadAsync();
            GoogleCredentials creds = Vault.Load();

            AccountText.Text = creds == null
                ? "Accesso Google non impostato. Apri \"Accesso Google\"."
                : "Account: " + (creds.Email ?? "(sconosciuto)");

            ContactsText.Text = "Contatti: " + state.ContactsCount + " sincronizzati"
                + (state.LastContactsSync.HasValue ? ", ultimo aggiornamento " + state.LastContactsSync.Value.LocalDateTime.ToString("dd/MM/yyyy HH:mm") : "")
                + (ContactsSwitch.IsOn ? "" : " (disattivato)");

            CalendarText.Text = "Calendario: " + state.CalendarCount + " eventi"
                + (state.LastCalendarSync.HasValue ? ", ultimo aggiornamento " + state.LastCalendarSync.Value.LocalDateTime.ToString("dd/MM/yyyy HH:mm") : "")
                + (CalendarSwitch.IsOn ? "" : " (disattivato)");

            var log = new StringBuilder();

            BackgroundAccessStatus access = BackgroundExecutionManager.GetAccessStatus();
            bool timerRegistered = BackgroundTaskRegistration.AllTasks.Values.Any(t => t.Name == BackgroundRegistration.TaskName);
            log.AppendLine("Sfondo: accesso " + AccessStatusText(access) + ".");
            log.AppendLine("Timer SincroTimer: " + (timerRegistered ? "registrato" : "non registrato") + ".");
            if (!string.IsNullOrEmpty(BackgroundRegistration.StatusMessage))
            {
                log.AppendLine(BackgroundRegistration.StatusMessage);
            }

            if (state.LastBackgroundEnd.HasValue)
            {
                log.AppendLine("Ultima esecuzione in background: "
                    + state.LastBackgroundEnd.Value.LocalDateTime.ToString("dd/MM/yyyy HH:mm")
                    + "; risultato: " + (state.LastBackgroundResult ?? "(non disponibile)") + ".");
            }
            else if (state.LastBackgroundStart.HasValue)
            {
                log.AppendLine("Ultima esecuzione in background: avviata "
                    + state.LastBackgroundStart.Value.LocalDateTime.ToString("dd/MM/yyyy HH:mm")
                    + "; risultato: " + (state.LastBackgroundResult ?? "(non disponibile)") + ".");
            }
            else
            {
                log.AppendLine("Ultima esecuzione in background: mai.");
            }
            log.AppendLine("Esecuzioni in background: " + state.BackgroundRunCount);

            if (last != null)
            {
                if (last.ContactsText.Length > 0) log.AppendLine("Contatti: " + last.ContactsText);
                if (last.CalendarText.Length > 0) log.AppendLine("Calendario: " + last.CalendarText);
            }
            for (int i = Math.Max(0, state.Log.Count - 8); i < state.Log.Count; i++)
            {
                log.AppendLine(state.Log[i]);
            }
            LogText.Text = log.ToString();
        }

        private static string AccessStatusText(BackgroundAccessStatus status)
        {
            switch (status)
            {
                case BackgroundAccessStatus.AllowedMayUseActiveRealTimeConnectivity:
                    return "Permesso";
                case BackgroundAccessStatus.Denied:
                    return "Denied (negato)";
                case BackgroundAccessStatus.Unspecified:
                    return "Unspecified (non specificato)";
                default:
                    return status.ToString();
            }
        }
    }
}
