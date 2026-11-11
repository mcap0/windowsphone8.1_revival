using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Claude.Core;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Claude
{
    /// <summary>A message as shown in the list; the text of the answer grows while it is streamed.</summary>
    public sealed class MessageView : INotifyPropertyChanged
    {
        string text = "", status = "";
        public ChatMessage Message { get; }
        public MessageView(ChatMessage message) { Message = message; text = message.Text ?? ""; }

        public bool IsUser => Message.IsUser;

        public string Text
        {
            get => text;
            set { text = value; Message.Text = value; Changed(nameof(Text)); }
        }

        public string Status
        {
            get => status;
            set { status = value; Changed(nameof(Status)); Changed(nameof(StatusVisibility)); }
        }

        public Visibility StatusVisibility => string.IsNullOrEmpty(status) ? Visibility.Collapsed : Visibility.Visible;

        public event PropertyChangedEventHandler PropertyChanged;
        void Changed(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    public sealed class MessageTemplateSelector : DataTemplateSelector
    {
        public DataTemplate User { get; set; }
        public DataTemplate Assistant { get; set; }
        protected override DataTemplate SelectTemplateCore(object item) => (item as MessageView)?.IsUser == true ? User : Assistant;
        protected override DataTemplate SelectTemplateCore(object item, DependencyObject container) => SelectTemplateCore(item);
    }

    public sealed partial class MainPage : Page
    {
        readonly ObservableCollection<MessageView> items = new ObservableCollection<MessageView>();
        readonly ClaudeApi api = new ClaudeApi();
        Conversation conversation;
        CancellationTokenSource sending;

        /// <summary>Set by ChatsPage before going back: the id of the conversation to show.</summary>
        public static string RequestedConversation;

        public MainPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;
            Messages.ItemsSource = items;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            var history = await Settings.LoadHistoryAsync();
            var requested = RequestedConversation;
            RequestedConversation = null;
            if (requested != null) Show(history.Find(requested));
            else if (conversation == null) Show(history.Conversations.FirstOrDefault());
            else if (!history.Conversations.Contains(conversation)) Show(null); // deleted
            if (Settings.ApiKey == null)
                EmptyText.Text = "Per iniziare inserisci la tua chiave API di Anthropic (console.anthropic.com) nelle Impostazioni.";
        }

        void Show(Conversation c)
        {
            if (sending != null) return;
            conversation = c;
            items.Clear();
            if (c != null) foreach (var m in c.Messages) items.Add(new MessageView(m));
            TitleText.Text = c == null || string.IsNullOrEmpty(c.Title) ? "Claude" : c.Title;
            UpdateEmpty();
            ScrollToEnd();
        }

        void UpdateEmpty() => EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        void ScrollToEnd()
        {
            if (items.Count > 0) Messages.ScrollIntoView(items[items.Count - 1]);
        }

        void OnInputKeyDown(object sender, KeyRoutedEventArgs e)
        {
            // Enter on a hardware keyboard sends, Shift+Enter makes a new line
            if (e.Key == VirtualKey.Enter && !Window.Current.CoreWindow.GetKeyState(VirtualKey.Shift).HasFlag(CoreVirtualKeyStates.Down)
                && Windows.System.Profile.AnalyticsInfo.VersionInfo.DeviceFamily != "Windows.Mobile")
            {
                e.Handled = true;
                OnSend(sender, e);
            }
        }

        async void OnSend(object sender, RoutedEventArgs e)
        {
            if (sending != null)
            {
                sending.Cancel(); // the button stops the answer being written
                return;
            }
            var question = Input.Text.Trim();
            if (question.Length == 0) return;
            var key = Settings.ApiKey;
            if (key == null)
            {
                Frame.Navigate(typeof(SettingsPage));
                return;
            }

            var history = await Settings.LoadHistoryAsync();
            if (conversation == null)
            {
                conversation = new Conversation { Title = Conversation.TitleFor(question) };
                TitleText.Text = conversation.Title;
            }
            var userMessage = new ChatMessage { Role = "user", Text = question };
            conversation.Messages.Add(userMessage);
            items.Add(new MessageView(userMessage));
            var answer = new MessageView(new ChatMessage { Role = "assistant", Text = "" }) { Status = "Claude sta scrivendo…" };
            items.Add(answer);
            Input.Text = "";
            UpdateEmpty();
            ScrollToEnd();
            history.Touch(conversation);

            sending = new CancellationTokenSource();
            SendIcon.Symbol = Symbol.Stop;
            var request = ClaudeApi.BuildRequest(Settings.Model, Settings.SystemPrompt, conversation.Messages, Settings.WebSearch);
            var listener = new Listener(this, answer);
            try
            {
                var result = await Task.Run(() => api.SendAsync(key, request, listener, sending.Token));
                await listener.FlushAsync();
                answer.Status = result.StopReason == "max_tokens" ? "(risposta interrotta: troppo lunga)" : "";
                conversation.Messages.Add(answer.Message);
            }
            catch (OperationCanceledException)
            {
                await listener.FlushAsync();
                answer.Status = "(interrotta)";
                if (answer.Text.Length > 0) conversation.Messages.Add(answer.Message);
                else items.Remove(answer);
            }
            catch (Exception ex)
            {
                await listener.FlushAsync();
                items.Remove(answer);
                // the question stays in the box to be sent again
                conversation.Messages.Remove(userMessage);
                items.Remove(items.LastOrDefault(i => i.Message == userMessage));
                Input.Text = question;
                await new MessageDialog(ex is ClaudeException ? ex.Message : "Errore di rete: " + ex.Message, "Claude").ShowAsync();
            }
            finally
            {
                sending = null;
                SendIcon.Symbol = Symbol.Send;
                if (conversation.Messages.Count == 0) history.Conversations.Remove(conversation);
                UpdateEmpty();
                await Settings.SaveHistoryAsync();
            }
        }

        /// <summary>Receives the streamed answer on a worker thread and shows it a few times per second.</summary>
        sealed class Listener : IStreamListener
        {
            readonly MainPage page;
            readonly MessageView answer;
            readonly System.Text.StringBuilder pending = new System.Text.StringBuilder();
            int scheduled;

            public Listener(MainPage page, MessageView answer) { this.page = page; this.answer = answer; }

            public void OnText(string text)
            {
                lock (pending) pending.Append(text);
                if (Interlocked.Exchange(ref scheduled, 1) == 0)
                    _ = Task.Delay(150).ContinueWith(t => FlushAsync());
            }

            public void OnSearch(string query) =>
                _ = page.Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => answer.Status = "Cerco sul web: " + query);

            public async Task FlushAsync()
            {
                await page.Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    Interlocked.Exchange(ref scheduled, 0);
                    string text;
                    lock (pending) { text = pending.ToString(); pending.Clear(); }
                    if (text.Length == 0) return;
                    answer.Text += text;
                    if (answer.Status.StartsWith("Claude sta") || answer.Status.StartsWith("Cerco")) answer.Status = "";
                    page.ScrollToEnd();
                });
            }
        }

        void OnNewChat(object sender, RoutedEventArgs e) => Show(null);

        void OnChats(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(ChatsPage));

        void OnSettings(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(SettingsPage));

        void OnCopyLast(object sender, RoutedEventArgs e)
        {
            var last = items.LastOrDefault(i => !i.IsUser);
            if (last == null) return;
            var package = new DataPackage();
            package.SetText(last.Text);
            Clipboard.SetContent(package);
        }
    }
}
