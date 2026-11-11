/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The download queue: progress, the saved file, and a retry for the failed items.
 */

using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Perle81.App
{
    public sealed partial class DownloadPage : Page
    {
        public DownloadPage()
        {
            InitializeComponent();
            List.ItemsSource = DownloadService.Items;
            EmptyText.Visibility = DownloadService.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        void OnRetry(object sender, RoutedEventArgs e)
        {
            var button = sender as Button;
            var item = button == null ? null : button.DataContext as DownloadItem;
            if (item != null) DownloadService.Retry(item);
        }
    }
}
