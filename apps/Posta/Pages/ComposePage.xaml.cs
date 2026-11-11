using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using MimeKit;
using Posta.Core.Logic;
using Posta.Core.Mail;
using Posta.Core.Model;
using Posta.Services;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Posta.Pages
{
    public sealed partial class ComposePage : Page
    {
        private MailContent _replyTo;
        private bool _busy;
        private readonly ObservableCollection<OutgoingAttachment> _attachments = new ObservableCollection<OutgoingAttachment>();
        private const long MaxAttachmentBytes = 20L * 1024 * 1024;

        public ComposePage()
        {
            InitializeComponent();
            // kept alive so the text survives a trip to FilePage; a new compose resets it in OnNavigatedTo
            NavigationCacheMode = NavigationCacheMode.Required;
            AttachList.ItemsSource = _attachments;
        }

        private void AttachButton_Click(object sender, RoutedEventArgs e)
        {
            if (_busy)
                return;
            Frame.Navigate(typeof(FilePage));
        }

        private void AddAttachment(string path)
        {
            var name = path.Substring(path.LastIndexOf('\\') + 1);
            try
            {
                var data = PublicFiles.Read(path);
                long total = _attachments.Sum(a => (long)a.Data.Length) + data.Length;
                if (total > MaxAttachmentBytes)
                {
                    ShowError("Allegati troppo grandi (massimo 20 MB in tutto).");
                    return;
                }
                _attachments.Add(new OutgoingAttachment { Name = name, Data = data });
                StatusText.Visibility = Visibility.Collapsed;
            }
            catch (Exception ex)
            {
                ShowError("Impossibile leggere " + name + ": " + ex.Message);
            }
        }

        private async void Attachment_Tapped(object sender, Windows.UI.Xaml.Input.TappedRoutedEventArgs e)
        {
            var attachment = (sender as FrameworkElement)?.DataContext as OutgoingAttachment;
            if (attachment == null || _busy)
                return;
            var dialog = new MessageDialog(attachment.Name, "Togliere l'allegato?");
            dialog.Commands.Add(new UICommand("togli"));
            dialog.Commands.Add(new UICommand("annulla"));
            var chosen = await dialog.ShowAsync();
            if (chosen != null && chosen.Label == "togli")
                _attachments.Remove(attachment);
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            if (e.NavigationMode == NavigationMode.Back)
            {
                var paths = ComposeRequest.PendingAttachments;
                ComposeRequest.PendingAttachments = null;
                if (paths != null)
                    foreach (var path in paths)
                        AddAttachment(path);
                return;
            }

            TitleText.Text = "Nuovo messaggio";
            ToBox.Text = CcBox.Text = SubjectBox.Text = BodyBox.Text = "";
            _attachments.Clear();
            StatusText.Visibility = Visibility.Collapsed;
            ComposeRequest.PendingAttachments = null;

            var mode = ComposeRequest.TakeMode();
            _replyTo = ComposeRequest.Take();
            if (_replyTo == null)
                return;

            var original = string.IsNullOrWhiteSpace(_replyTo.TextBody)
                ? ReplyText.PlainFromHtml(_replyTo.HtmlBody)
                : _replyTo.TextBody;
            var dateText = _replyTo.Date == DateTimeOffset.MinValue
                ? "(data sconosciuta)"
                : _replyTo.Date.ToLocalTime().ToString("dd/MM/yyyy HH:mm");

            if (mode == ComposeRequest.Mode.Forward)
            {
                TitleText.Text = "Inoltra messaggio";
                SubjectBox.Text = ReplyText.ForwardSubject(_replyTo.Subject);
                BodyBox.Text = ReplyText.Quote(original, _replyTo.From, dateText);
                return;
            }

            ToBox.Text = ReplyText.ExtractAddress(_replyTo.From);
            SubjectBox.Text = ReplyText.ReplySubject(_replyTo.Subject);
            BodyBox.Text = ReplyText.Quote(original, _replyTo.From, dateText);

            if (mode == ComposeRequest.Mode.ReplyAll)
            {
                TitleText.Text = "Rispondi a tutti";
                var recipients = new List<string>();
                AddOtherRecipients(recipients, _replyTo.To);
                AddOtherRecipients(recipients, _replyTo.Cc);
                CcBox.Text = string.Join(", ", recipients.ToArray());
            }
            else
            {
                TitleText.Text = "Rispondi";
            }
        }

        private static void AddOtherRecipients(List<string> result, string raw)
        {
            if (string.IsNullOrWhiteSpace(raw))
                return;

            var account = AccountStore.Load();
            try
            {
                var parsed = InternetAddressList.Parse(raw);
                foreach (var address in parsed)
                {
                    var mailbox = address as MailboxAddress;
                    if (mailbox == null ||
                        account == null ||
                        !string.Equals(mailbox.Address, account.Email, StringComparison.OrdinalIgnoreCase))
                    {
                        result.Add(address.ToString());
                    }
                }
            }
            catch (ParseException)
            {
                result.Add(raw);
            }
        }

        private async void SendButton_Click(object sender, RoutedEventArgs e)
        {
            if (_busy)
                return;

            var account = AccountStore.Load();
            var password = account == null ? null : AccountStore.LoadPassword(account.Email);
            if (account == null || password == null)
            {
                ShowError("Account non configurato.");
                return;
            }

            if (string.IsNullOrWhiteSpace(ToBox.Text))
            {
                ShowError("Indica un destinatario.");
                return;
            }

            _busy = true;
            SetBusy(true);
            StatusText.Visibility = Visibility.Collapsed;
            try
            {
                using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(_attachments.Count > 0 ? 300 : 90)))
                {
                    await new MailService(account, password).SendAsync(
                        ToBox.Text.Trim(),
                        CcBox.Text.Trim(),
                        SubjectBox.Text.Trim(),
                        BodyBox.Text,
                        _replyTo != null ? _replyTo.MessageId : null,
                        _replyTo != null ? ReplyText.ReferencesFor(_replyTo) : null,
                        _attachments.ToList(),
                        cts.Token);
                }

                if (Frame.CanGoBack)
                    Frame.GoBack();
            }
            catch (OperationCanceledException)
            {
                ShowError("Invio annullato.");
            }
            catch (Exception ex)
            {
                ShowError(MailService.Describe(ex));
            }
            finally
            {
                _busy = false;
                SetBusy(false);
            }
        }

        private void ShowError(string text)
        {
            StatusText.Text = text;
            StatusText.Visibility = Visibility.Visible;
        }

        private void SetBusy(bool busy)
        {
            Busy.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
            Busy.IsIndeterminate = busy;
            SendButton.IsEnabled = !busy;
            AttachButton.IsEnabled = !busy;
        }
    }
}
