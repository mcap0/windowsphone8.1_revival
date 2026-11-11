using System;
using System.Linq;
using Claude.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Claude
{
    public sealed class ChatRow
    {
        public Conversation Conversation { get; set; }
        public string Title => string.IsNullOrEmpty(Conversation.Title) ? "(senza titolo)" : Conversation.Title;
        public string Details => DateTimeOffset.FromUnixTimeMilliseconds(Conversation.Updated).ToLocalTime().ToString("dd/MM/yyyy HH:mm")
            + " · " + Conversation.Messages.Count + " messaggi";
    }

    /// <summary>The saved conversations: tap to open, hold to delete.</summary>
    public sealed partial class ChatsPage : Page
    {
        public ChatsPage() { InitializeComponent(); }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            var history = await Settings.LoadHistoryAsync();
            List.ItemsSource = history.Conversations.Select(c => new ChatRow { Conversation = c }).ToList();
            EmptyText.Visibility = history.Conversations.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        void OnClick(object sender, ItemClickEventArgs e)
        {
            MainPage.RequestedConversation = ((ChatRow)e.ClickedItem).Conversation.Id;
            if (Frame.CanGoBack) Frame.GoBack();
        }

        void OnHolding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started) ShowMenu(e.OriginalSource as FrameworkElement);
        }

        void OnRightTapped(object sender, RightTappedRoutedEventArgs e) => ShowMenu(e.OriginalSource as FrameworkElement);

        void ShowMenu(FrameworkElement source)
        {
            if (!(source?.DataContext is ChatRow row)) return;
            var menu = new MenuFlyout();
            var delete = new MenuFlyoutItem { Text = "Elimina" };
            delete.Click += async (s, a) =>
            {
                var history = await Settings.LoadHistoryAsync();
                history.Conversations.Remove(row.Conversation);
                await Settings.SaveHistoryAsync();
                OnNavigatedTo(null);
            };
            menu.Items.Add(delete);
            menu.ShowAt(source);
        }
    }
}
