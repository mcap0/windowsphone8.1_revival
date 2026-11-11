using System;
using Sincro.Core;
using Sincro.Logic;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Sincro
{
    public sealed partial class SetupPage : Page
    {
        public SetupPage()
        {
            this.InitializeComponent();
        }

        private void Page_Loaded(object sender, RoutedEventArgs e)
        {
            AddressBox.Text = SetupService.DefaultAddress;
            ShowAccount();
        }

        private void ShowAccount()
        {
            GoogleCredentials creds = Vault.Load();
            StatusText.Text = creds == null
                ? "Non connesso."
                : "Connesso come " + (creds.Email ?? "(account)") + ".";
        }

        private async void Import_Click(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            StatusText.Text = "Importazione in corso...";
            try
            {
                string email = await SetupService.ImportAsync(AddressBox.Text.Trim());
                StatusText.Text = "Accesso riuscito come " + email + ".";
            }
            catch (Exception ex)
            {
                StatusText.Text = "";
                ErrorText.Text = "Importazione fallita: " + ex.Message;
            }
        }

        private void Disconnect_Click(object sender, RoutedEventArgs e)
        {
            Vault.Clear();
            ShowAccount();
        }

        private async void Wipe_Click(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            StatusText.Text = "Rimozione in corso...";
            try
            {
                await SyncEngine.RemoveSyncedDataAsync();
                StatusText.Text = "Contatti e calendario di Sincro rimossi. L'account resta collegato.";
            }
            catch (Exception ex)
            {
                ErrorText.Text = "Rimozione fallita: " + ex.Message;
            }
        }
    }
}
