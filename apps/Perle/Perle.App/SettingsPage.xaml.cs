/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of CastSettingsFragment: the "Cast to TV" settings and its log.
 */

using Perle.Cast;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Perle.App
{
    public sealed partial class SettingsPage : Page
    {
        public SettingsPage()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            DirectLinkSwitch.IsOn = CastSettings.DirectLink;
            ShowLog();
        }

        void ShowLog()
        {
            // the newest entries: the whole log is too long for a TextBlock
            var log = CastLog.DumpRecent();
            LogText.Text = log.Length == 0 ? CastStrings.LogEmpty : log;
        }

        void OnDirectLinkToggled(object sender, RoutedEventArgs e) => CastSettings.DirectLink = DirectLinkSwitch.IsOn;

        void OnCopy(object sender, RoutedEventArgs e) => CastControllerPage.CopyToClipboard(CastLog.Dump());

        void OnClear(object sender, RoutedEventArgs e)
        {
            CastLog.Clear();
            ShowLog();
        }
    }
}
