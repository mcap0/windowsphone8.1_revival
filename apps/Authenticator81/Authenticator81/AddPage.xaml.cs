using System;
using System.Collections.Generic;
using Authenticator.Core;
using Windows.ApplicationModel.Activation;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Authenticator
{
    public sealed partial class AddPage : Page, IFileOpenPickerContinuable
    {
        public AddPage()
        {
            InitializeComponent();
        }

        void OnSave(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            try
            {
                OtpAccount account;
                if (!string.IsNullOrWhiteSpace(UriBox.Text))
                {
                    account = OtpUri.Parse(UriBox.Text);
                }
                else
                {
                    if (string.IsNullOrWhiteSpace(SecretBox.Text))
                        throw new FormatException("Inserisci il codice segreto.");

                    account = new OtpAccount
                    {
                        Issuer = IssuerBox.Text.Trim(),
                        Label = LabelBox.Text.Trim(),
                        Secret = Base32.Decode(SecretBox.Text)
                    };

                    if (account.Secret.Length == 0)
                        throw new FormatException("Codice segreto vuoto.");
                }

                var all = App.Store.Load();
                all.Add(account);
                App.Store.Save(account, all);
                Frame.GoBack();
            }
            catch (Exception ex)
            {
                ErrorText.Text = ex.Message;
            }
        }

        void OnScan(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(ScanPage));
        }

        void OnPickPhoto(object sender, RoutedEventArgs e)
        {
            try
            {
                App.SetPickerTarget("add");
                var picker = new FileOpenPicker();
                picker.SuggestedStartLocation = PickerLocationId.PicturesLibrary;
                picker.ViewMode = PickerViewMode.Thumbnail;
                picker.FileTypeFilter.Add(".jpg");
                picker.FileTypeFilter.Add(".jpeg");
                picker.FileTypeFilter.Add(".png");
                picker.FileTypeFilter.Add(".bmp");
                picker.PickSingleFileAndContinue();
            }
            catch (Exception ex)
            {
                App.ClearPickerTarget();
                StatusText.Text = "Impossibile aprire le foto: " + ex.Message;
            }
        }

        public async void ContinueFileOpenPicker(FileOpenPickerContinuationEventArgs args)
        {
            App.ClearPickerTarget();
            if (args.Files == null || args.Files.Count == 0)
            {
                StatusText.Text = "Nessuna foto selezionata.";
                return;
            }

            try
            {
                StatusText.Text = "Lettura del QR…";
                string text = await QrCodeDecoder.DecodeFileAsync(args.Files[0]);
                if (string.IsNullOrEmpty(text))
                {
                    StatusText.Text = "Nessun QR leggibile nella foto.";
                    return;
                }

                var result = OtpUri.ParseImport(text);
                int imported = await AccountImport.ImportAsync(result);
                if (imported < 0)
                {
                    StatusText.Text = "Importazione annullata.";
                    return;
                }

                if (result.IsMigration && result.BatchSize > 1)
                {
                    StatusText.Text = "QR " + (result.BatchIndex + 1) + " di " + result.BatchSize
                        + " importato, scansiona il prossimo.";
                }
                else if (imported == 0)
                {
                    StatusText.Text = "Gli account trovati sono già presenti.";
                }
                else
                {
                    Frame.GoBack();
                }
            }
            catch (Exception ex)
            {
                StatusText.Text = ex.Message;
            }
        }
    }
}
