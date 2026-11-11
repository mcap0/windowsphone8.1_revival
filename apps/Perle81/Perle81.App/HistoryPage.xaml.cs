/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The videos watched in Perle, newest first.
 */

using System.Collections.ObjectModel;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class HistoryPage : Page
    {
        readonly ObservableCollection<HistoryEntry> entries = new ObservableCollection<HistoryEntry>();

        public HistoryPage()
        {
            InitializeComponent();
            List.ItemsSource = entries;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            entries.Clear();
            foreach (var h in await AppState.HistoryAsync()) entries.Add(h);
            EmptyText.Visibility = entries.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        void OnVideoClick(object sender, ItemClickEventArgs e)
        {
            var entry = e.ClickedItem as HistoryEntry;
            if (entry != null) Frame.Navigate(typeof(PlayerPage), entry.Url);
        }
    }
}
