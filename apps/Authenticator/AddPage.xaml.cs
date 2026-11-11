using System;
using System.Linq;
using Authenticator.Core;
using Windows.Media.Capture;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Authenticator
{
    public sealed partial class AddPage : Page
    {
        public AddPage() { InitializeComponent(); }

        async void OnScan(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            try
            {
                var capture = new CameraCaptureUI();
                capture.PhotoSettings.Format = CameraCaptureUIPhotoFormat.Jpeg;
                capture.PhotoSettings.AllowCropping = false;
                var photo = await capture.CaptureFileAsync(CameraCaptureUIMode.Photo);
                if (photo == null) return;
                var text = await QrDecoder.DecodeAsync(photo);
                await photo.DeleteAsync();
                if (text == null) { ErrorText.Text = "QR code non trovato. Riprova più vicino e con più luce."; return; }
                if (text.StartsWith("otpauth-migration://", StringComparison.OrdinalIgnoreCase))
                {
                    ErrorText.Text = "È un export di Google Authenticator: per ora esporta gli account uno alla volta.";
                    return;
                }
                Add(OtpUri.Parse(text));
            }
            catch (Exception ex) { ErrorText.Text = ex.Message; }
        }

        void OnSave(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            try
            {
                OtpAccount a;
                if (!string.IsNullOrWhiteSpace(UriBox.Text)) a = OtpUri.Parse(UriBox.Text);
                else
                {
                    if (string.IsNullOrWhiteSpace(SecretBox.Text)) throw new FormatException("Inserisci il codice segreto.");
                    a = new OtpAccount
                    {
                        Issuer = IssuerBox.Text.Trim(),
                        Label = LabelBox.Text.Trim(),
                        Secret = Base32.Decode(SecretBox.Text)
                    };
                    if (a.Secret.Length == 0) throw new FormatException("Codice segreto vuoto.");
                }
                Add(a);
            }
            catch (Exception ex) { ErrorText.Text = ex.Message; }
        }

        void Add(OtpAccount a)
        {
            var all = MainPage.Store.Load();
            all.Add(a);
            MainPage.Store.Save(a, all);
            Frame.GoBack();
        }
    }
}
