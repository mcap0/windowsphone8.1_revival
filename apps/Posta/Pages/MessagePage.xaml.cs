using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Posta.Core.Logic;
using Posta.Core.Mail;
using Posta.Core.Model;
using Posta.Services;
using Windows.ApplicationModel.Activation;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Posta.Pages
{
    public sealed partial class MessagePage : Page
    {
        private const string PendingAttachmentFile = "pending-attachment.bin";
        private const string PendingAttachmentSetting = "posta.pending-attachment.v1";

        private uint _uid;
        private string _folderKey = MailFolderKeys.Inbox;
        private MailContent _content;
        private MailMessageDetails _details;
        private bool _allowImages;
        private bool _busy;

        public MessagePage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Disabled;
            ImagesToggle.IsChecked = false;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            var rawKey = e.Parameter as string;
            if (string.IsNullOrEmpty(rawKey))
            {
                var legacyUid = e.Parameter is uint ? (uint)e.Parameter : 0;
                if (legacyUid > 0)
                    _uid = legacyUid;
            }
            else
            {
                var separator = rawKey.LastIndexOf('|');
                var uidText = separator >= 0 ? rawKey.Substring(separator + 1) : rawKey;
                uint parsedUid;
                if (!uint.TryParse(uidText, out parsedUid))
                {
                    StatusText.Text = "Messaggio non valido.";
                    return;
                }

                _uid = parsedUid;
                if (separator >= 0)
                    _folderKey = rawKey.Substring(0, separator);
            }

            if (_uid == 0)
            {
                StatusText.Text = "Messaggio non valido.";
                return;
            }

            await LoadAsync();
        }

        private async Task LoadAsync()
        {
            var account = AccountStore.Load();
            var password = account == null ? null : AccountStore.LoadPassword(account.Email);
            if (account == null || password == null)
            {
                StatusText.Text = "Account non configurato.";
                return;
            }

            await RunAsync(async ct =>
            {
                _details = await new MailService(account, password)
                    .FetchMessageDetailsAsync(_folderKey, _uid, ct);
                _content = _details.Content;
                ShowContent();
                StatusText.Text = "";
            });
        }

        private void ShowContent()
        {
            if (_content == null)
                return;

            SubjectText.Text = string.IsNullOrWhiteSpace(_content.Subject)
                ? "(nessun oggetto)"
                : _content.Subject;
            FromText.Text = "Da: " + _content.From;
            ToText.Text = "A: " + _content.To;
            if (!string.IsNullOrWhiteSpace(_content.Cc))
                ToText.Text += "\nCc: " + _content.Cc;

            DateText.Text = _content.Date == DateTimeOffset.MinValue
                ? ""
                : _content.Date.ToLocalTime().ToString("dd/MM/yyyy HH:mm");

            var body = BodySelector.Choose(_content.TextBody, _content.HtmlBody);
            var dark = Application.Current.RequestedTheme == ApplicationTheme.Dark;
            // plain text goes through the WebView too, so its links are clickable like in HTML mail
            PlainScroll.Visibility = Visibility.Collapsed;
            HtmlView.Visibility = Visibility.Visible;
            if (body.Kind == BodyKind.Html)
            {
                HtmlView.NavigateToString(HtmlPreparer.Prepare(body.Text, _allowImages, dark));

                var blocked = _allowImages ? 0 : HtmlPreparer.CountRemoteImages(body.Text);
                BlockedText.Text = "Immagini remote bloccate: " + blocked + ".";
                BlockedText.Visibility = blocked > 0 ? Visibility.Visible : Visibility.Collapsed;
                ImagesToggle.IsEnabled = true;
            }
            else
            {
                BlockedText.Visibility = Visibility.Collapsed;
                var text = body.Kind == BodyKind.Plain ? body.Text : "(messaggio senza testo)";
                HtmlView.NavigateToString(HtmlPreparer.Prepare(HtmlPreparer.PlainToHtml(text), false, dark));
                ImagesToggle.IsEnabled = false;
            }

            var attachments = new List<AttachmentViewItem>();
            if (_details.Attachments != null)
            {
                for (var i = 0; i < _details.Attachments.Count; i++)
                {
                    var info = _details.Attachments[i];
                    attachments.Add(new AttachmentViewItem
                    {
                        Index = i,
                        Name = string.IsNullOrWhiteSpace(info.Name) ? "Allegato" : info.Name,
                        SizeText = info.Size > 0 ? FormatSize(info.Size) : "Dimensione non disponibile"
                    });
                }
            }

            AttachmentsList.ItemsSource = attachments;
            AttachmentsList.Visibility = attachments.Count > 0
                ? Visibility.Visible
                : Visibility.Collapsed;
        }

        private async Task RunAsync(Func<CancellationToken, Task> operation)
        {
            if (_busy)
                return;

            _busy = true;
            SetBusy(true);
            try
            {
                using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(90)))
                    await operation(cts.Token);
            }
            catch (OperationCanceledException)
            {
                StatusText.Text = "Operazione annullata.";
            }
            catch (Exception ex)
            {
                StatusText.Text = MailService.Describe(ex);
            }
            finally
            {
                _busy = false;
                SetBusy(false);
            }
        }

        private void SetBusy(bool busy)
        {
            Busy.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
            Busy.IsIndeterminate = busy;
            ReplyButton.IsEnabled = !busy && _content != null;
            ReplyAllButton.IsEnabled = !busy && _content != null;
            ForwardButton.IsEnabled = !busy && _content != null;
            DeleteButton.IsEnabled = !busy;
            UnreadButton.IsEnabled = !busy;
        }

        private void NavigateToCompose(ComposeRequest.Mode mode)
        {
            if (_content == null)
                return;

            ComposeRequest.Start(mode, _content);
            Frame.Navigate(typeof(ComposePage));
        }

        private void ReplyButton_Click(object sender, RoutedEventArgs e)
        {
            NavigateToCompose(ComposeRequest.Mode.Reply);
        }

        private void ReplyAllButton_Click(object sender, RoutedEventArgs e)
        {
            NavigateToCompose(ComposeRequest.Mode.ReplyAll);
        }

        private void ForwardButton_Click(object sender, RoutedEventArgs e)
        {
            NavigateToCompose(ComposeRequest.Mode.Forward);
        }

        private async void DeleteButton_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new MessageDialog("Spostare questo messaggio nel Cestino?");
            dialog.Commands.Add(new UICommand("Sì"));
            dialog.Commands.Add(new UICommand("No"));
            dialog.DefaultCommandIndex = 0;
            dialog.CancelCommandIndex = 1;

            var choice = await dialog.ShowAsync();
            if (choice.Label != "Sì")
                return;

            var account = AccountStore.Load();
            var password = account == null ? null : AccountStore.LoadPassword(account.Email);
            if (account == null || password == null)
                return;

            await RunAsync(async ct =>
            {
                await new MailService(account, password).DeleteAsync(_folderKey, _uid, ct);
                if (Frame.CanGoBack)
                    Frame.GoBack();
            });
        }

        private async void UnreadButton_Click(object sender, RoutedEventArgs e)
        {
            var account = AccountStore.Load();
            var password = account == null ? null : AccountStore.LoadPassword(account.Email);
            if (account == null || password == null)
                return;

            await RunAsync(async ct =>
            {
                await new MailService(account, password).SetSeenAsync(_folderKey, _uid, false, ct);
                StatusText.Text = "Segnato come non letto.";
            });
        }

        private void ImagesToggle_Click(object sender, RoutedEventArgs e)
        {
            _allowImages = ImagesToggle.IsChecked == true;
            ShowContent();
        }

        private async void Attachment_ItemClick(object sender, ItemClickEventArgs e)
        {
            var attachment = e.ClickedItem as AttachmentViewItem;
            if (attachment == null || _busy)
                return;

            var account = AccountStore.Load();
            var password = account == null ? null : AccountStore.LoadPassword(account.Email);
            if (account == null || password == null)
                return;

            await RunAsync(async ct =>
            {
                var bytes = await new MailService(account, password)
                    .FetchAttachmentAsync(_folderKey, _uid, attachment.Index, ct);
                if (bytes == null)
                    throw new InvalidOperationException("Allegato vuoto.");
                var path = PublicFiles.SaveDownload(attachment.Name, bytes);
                StatusText.Text = "Salvato in Download\\Posta.";
                var dialog = new MessageDialog(path.Substring(path.LastIndexOf('\\') + 1), "Salvato in Download\\Posta");
                dialog.Commands.Add(new UICommand("apri"));
                dialog.Commands.Add(new UICommand("ok"));
                var chosen = await dialog.ShowAsync();
                if (chosen != null && chosen.Label == "apri" && !await PublicFiles.OpenAsync(attachment.Name, bytes))
                    StatusText.Text = "Nessuna app per aprire questo file (salvato in Download\\Posta).";
            });
        }

        public static async Task CompleteFileSaveAsync(FileSavePickerContinuationEventArgs args)
        {
            var settings = ApplicationData.Current.LocalSettings.Values;
            try
            {
                if (args == null || args.File == null)
                    return;

                object pendingMarker;
                if (!settings.TryGetValue(PendingAttachmentSetting, out pendingMarker))
                    return;

                var pending = await ApplicationData.Current.LocalFolder.GetFileAsync(PendingAttachmentFile);
                var buffer = await FileIO.ReadBufferAsync(pending);
                await FileIO.WriteBufferAsync(args.File, buffer);
                await pending.DeleteAsync();
            }
            catch (Exception)
            {
                // A cancelled or unavailable destination must not crash activation.
            }
            finally
            {
                settings.Remove(PendingAttachmentSetting);
                try
                {
                    var pending = await ApplicationData.Current.LocalFolder.GetFileAsync(PendingAttachmentFile);
                    await pending.DeleteAsync();
                }
                catch (Exception)
                {
                    // The temporary file may already have been removed.
                }
            }
        }

        private void HtmlView_ScriptNotify(object sender, NotifyEventArgs e)
        {
            var value = e.Value ?? "";
            if (value.StartsWith("open:"))
                OpenLink(value.Substring(5));
            else if (value.StartsWith("menu:"))
                ShowLinkPanel(value.Substring(5));
        }

        private void HtmlView_NavigationStarting(WebView sender, WebViewNavigationStartingEventArgs args)
        {
            // NavigateToString starts with a null Uri; anything else is a link that escaped the script
            if (args.Uri == null)
                return;
            args.Cancel = true;
            OpenLink(args.Uri.AbsoluteUri);
        }

        private void ShowLinkPanel(string url)
        {
            LinkBox.Text = url;
            LinkPanel.Visibility = Visibility.Visible;
            LinkBox.Focus(FocusState.Programmatic);
            LinkBox.SelectAll();
        }

        private void LinkOpen_Click(object sender, RoutedEventArgs e)
        {
            var url = LinkBox.Text;
            LinkPanel.Visibility = Visibility.Collapsed;
            OpenLink(url);
        }

        private void LinkClose_Click(object sender, RoutedEventArgs e)
        {
            LinkPanel.Visibility = Visibility.Collapsed;
        }

        /// <summary>Back key closes the link panel first.</summary>
        public bool HandleBack()
        {
            if (LinkPanel.Visibility != Visibility.Visible)
                return false;
            LinkPanel.Visibility = Visibility.Collapsed;
            return true;
        }

        private static async void OpenLink(string url)
        {
            if (string.IsNullOrWhiteSpace(url))
                return;
            url = url.Trim();
            Uri target;
            if (url.StartsWith("mailto:", StringComparison.OrdinalIgnoreCase))
            {
                if (!Uri.TryCreate(url, UriKind.Absolute, out target))
                    return;
            }
            else
            {
                if (!url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) &&
                    !url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                    url = "http://" + url;
                // through the system: http(s) opens in IE
                if (!Uri.TryCreate(url, UriKind.Absolute, out target))
                    return;
            }
            try { await Windows.System.Launcher.LaunchUriAsync(target); }
            catch (Exception) { }
        }

        private static string FormatSize(long size)
        {
            if (size < 1024)
                return size + " B";
            if (size < 1024 * 1024)
                return (size / 1024.0).ToString("0.#") + " KB";
            return (size / (1024.0 * 1024.0)).ToString("0.#") + " MB";
        }

        public sealed class AttachmentViewItem
        {
            public int Index { get; set; }
            public string Name { get; set; }
            public string SizeText { get; set; }
        }
    }
}
