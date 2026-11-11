using System;
using System.Threading.Tasks;
using Windows.Security.Cryptography;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.Web.Http;

namespace Appunti
{
    public sealed partial class MainPage : Page
    {
        private const string DefaultUrl = "http://192.168.1.148:8765/clip.txt";
        private const string UrlSettingKey = "SourceUrl";

        // The first page load fetches the clipboard once per app start; later returns do not.
        private static bool _startupRefreshDone;

        public MainPage()
        {
            this.InitializeComponent();
            History.ItemsSource = ClipStore.Items;
            SourceBox.Text = ReadSourceUrl();
        }

        private async void Page_Loaded(object sender, RoutedEventArgs e)
        {
            await ClipStore.LoadAsync();
            if (!_startupRefreshDone)
            {
                _startupRefreshDone = true;
                await RefreshAsync();
            }
        }

        private async void Refresh_Click(object sender, RoutedEventArgs e)
        {
            await RefreshAsync();
        }

        private void SourceBox_LostFocus(object sender, RoutedEventArgs e)
        {
            SaveSourceUrl(SourceBox.Text.Trim());
        }

        private void NewText_Click(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(ItemPage), null);
        }

        private void History_ItemClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as ClipItem;
            if (item != null) Frame.Navigate(typeof(ItemPage), item.Text);
        }

        private async Task RefreshAsync()
        {
            string url = SourceBox.Text.Trim();
            SaveSourceUrl(url);
            ErrorText.Text = "";
            StatusText.Text = "Aggiornamento in corso...";
            RefreshButton.IsEnabled = false;
            try
            {
                string text = await FetchAsync(url);
                if (string.IsNullOrWhiteSpace(text))
                {
                    throw new InvalidOperationException("il server ha inviato un testo vuoto");
                }
                await ClipStore.AddAsync(text);
                StatusText.Text = "Aggiornato alle " + DateTime.Now.ToString("HH:mm:ss");
            }
            catch (Exception ex)
            {
                StatusText.Text = "";
                ErrorText.Text = "Errore: " + ex.Message;
            }
            finally
            {
                RefreshButton.IsEnabled = true;
            }
        }

        // GET the URL with a cache-busting query parameter and no-cache headers; body is UTF-8 text.
        private static async Task<string> FetchAsync(string url)
        {
            if (string.IsNullOrEmpty(url)) throw new ArgumentException("inserisci un URL");
            var uri = new Uri(url);
            if (uri.Scheme != "http" && uri.Scheme != "https") throw new ArgumentException("l'URL deve iniziare con http:// o https://");

            string separator = uri.Query.Length > 0 ? "&" : "?";
            var busted = new Uri(url + separator + "_=" + DateTime.UtcNow.Ticks);

            using (var client = new HttpClient())
            {
                var request = new HttpRequestMessage(HttpMethod.Get, busted);
                request.Headers.Add("Cache-Control", "no-cache");
                request.Headers.Add("Pragma", "no-cache");

                using (HttpResponseMessage response = await client.SendRequestAsync(request))
                {
                    if (!response.IsSuccessStatusCode)
                    {
                        throw new InvalidOperationException("HTTP " + (int)response.StatusCode);
                    }
                    IBuffer buffer = await response.Content.ReadAsBufferAsync();
                    return CryptographicBuffer.ConvertBinaryToString(BinaryStringEncoding.Utf8, buffer);
                }
            }
        }

        private static string ReadSourceUrl()
        {
            try
            {
                var saved = ApplicationData.Current.LocalSettings.Values[UrlSettingKey] as string;
                if (!string.IsNullOrWhiteSpace(saved)) return saved;
            }
            catch (Exception)
            {
                // Settings unavailable: fall back to the default.
            }
            return DefaultUrl;
        }

        private static void SaveSourceUrl(string url)
        {
            try
            {
                ApplicationData.Current.LocalSettings.Values[UrlSettingKey] = url;
            }
            catch (Exception)
            {
                // Not persisted this time; the field still holds the value.
            }
        }
    }
}
