/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The download queue: progress, the saved file, and a retry for the failed items.
 */

using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using System;
using System.Collections.Specialized;

namespace Perle81.App
{
    public sealed partial class DownloadPage : Page
    {
        public DownloadPage()
        {
            InitializeComponent();
            List.ItemsSource = DownloadService.Items;
            Loaded += OnLoaded;
            Unloaded += OnUnloaded;
            UpdateEmpty();
        }

        void OnLoaded(object sender, RoutedEventArgs e)
        {
            DownloadService.Items.CollectionChanged += OnItemsChanged;
            UpdateEmpty();
        }

        void OnUnloaded(object sender, RoutedEventArgs e)
        {
            DownloadService.Items.CollectionChanged -= OnItemsChanged;
        }

        void OnItemsChanged(object sender, NotifyCollectionChangedEventArgs e) { UpdateEmpty(); }

        void UpdateEmpty()
        {
            EmptyText.Visibility = DownloadService.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        void OnRetry(object sender, RoutedEventArgs e)
        {
            var button = sender as Button;
            var item = button == null ? null : button.DataContext as DownloadItem;
            if (item != null) DownloadService.Retry(item);
        }

        async void OnDelete(object sender, RoutedEventArgs e)
        {
            var button = sender as Button;
            var item = button == null ? null : button.DataContext as DownloadItem;
            if (item == null) return;
            try { await DownloadService.DeleteAsync(item); }
            catch (Exception ex) { item.Message = "Impossibile eliminare: " + ex.Message; }
        }
    }
}
