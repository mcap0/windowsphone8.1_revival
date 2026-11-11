/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Import of the NewPipe subscriptions export (JSON), opened by ContinuationManager after the file picker.
 */

using System;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class ImportPage : Page
    {
        public ImportPage()
        {
            InitializeComponent();
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            StorageFile file = AppState.PendingImport;
            AppState.PendingImport = null;
            if (file == null)
            {
                Finish("Nessun file scelto.");
                return;
            }
            try
            {
                string json = await FileIO.ReadTextAsync(file);
                await AppState.ImportAsync(json);
                Finish("Iscrizioni importate: " + file.Name + ". Le cartelle e i video verranno aggiornati.");
            }
            catch (Exception ex)
            {
                Finish("File non valido: " + ex.Message);
            }
        }

        void Finish(string text)
        {
            Busy.Visibility = Visibility.Collapsed;
            Message.Text = text;
            DoneButton.Visibility = Visibility.Visible;
        }

        void OnDone(object sender, RoutedEventArgs e)
        {
            // MainPage is reloaded because SubscriptionsVersion changed; the back stack is not needed any more
            Frame.Navigate(typeof(MainPage));
            Frame.BackStack.Clear();
        }
    }
}
