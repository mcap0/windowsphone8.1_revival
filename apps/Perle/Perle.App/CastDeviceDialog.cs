/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.CastDeviceDialog.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Perle.Cast;
using Perle.Cast.Dlna;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Perle.App
{
    /// <summary>
    /// Searches the TVs on the local network, lists them as they are found and starts casting on
    /// the chosen one.
    /// </summary>
    public sealed class CastDeviceDialog : ContentDialog
    {
        readonly CastItem item;
        readonly List<DlnaDevice> devices = new List<DlnaDevice>();
        readonly ObservableCollection<string> labels = new ObservableCollection<string>();
        readonly ProgressBar searching = new ProgressBar { IsIndeterminate = true, Margin = new Thickness(0, 4, 0, 8) };
        readonly TextBlock message = new TextBlock { TextWrapping = TextWrapping.Wrap, Text = CastStrings.NoDevices };
        readonly ListView list = new ListView { SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = true, MaxHeight = 360 };
        bool isSearching;

        /// <summary>The device chosen, null if the dialog was cancelled.</summary>
        public DlnaDevice Chosen { get; private set; }

        public CastDeviceDialog(CastItem item)
        {
            this.item = item;
            Title = CastStrings.CastToTv;
            list.ItemsSource = labels;
            list.ItemClick += OnItemClick;
            var panel = new StackPanel();
            panel.Children.Add(searching);
            panel.Children.Add(message);
            panel.Children.Add(list);
            Content = panel;
            PrimaryButtonText = CastStrings.Retry;
            SecondaryButtonText = "Annulla";
            // the retry button must not close the dialog
            PrimaryButtonClick += (s, e) => { e.Cancel = true; Search(); };
            Search();
        }

        /// <summary>Shows the dialog, and starts casting if a TV is chosen.</summary>
        public async Task ShowAndCastAsync(Frame frame)
        {
            await ShowAsync();
            if (Chosen == null) return;
            CastSettings.LastDeviceUdn = Chosen.Udn;
            CastService.Start(Chosen, item);
            frame.Navigate(typeof(CastControllerPage));
        }

        void OnItemClick(object sender, ItemClickEventArgs e)
        {
            int index = labels.IndexOf((string)e.ClickedItem);
            if (index < 0 || index >= devices.Count) return;
            Chosen = devices[index];
            Hide();
        }

        void Search()
        {
            if (isSearching) return;
            devices.Clear();
            labels.Clear();
            isSearching = true;
            UpdateViews();
            var dispatcher = Dispatcher;
            Task.Run(() =>
            {
                var local = LocalNetwork.Find();
                if (local == null)
                {
                    CastLog.W("No local network address: the phone is not on Wi-Fi");
                    return (string)CastStrings.ErrorNoWifi;
                }
                CastLog.D("Searching TVs on " + local.InterfaceName + " (" + local.Address + ")");
                try
                {
                    // devices are found on several threads at once: added in order on the UI thread
                    DlnaDiscovery.Discover(local.Address, device =>
                        _ = dispatcher.RunAsync(CoreDispatcherPriority.Normal, () => AddDevice(device)));
                    return null;
                }
                catch (Exception e)
                {
                    CastLog.E("Search failed", e);
                    return CastStrings.NoDevices;
                }
            }).ContinueWith(t => dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                isSearching = false;
                CastLog.D("Search finished, " + devices.Count + " renderer(s) found");
                UpdateViews();
                if (t.Result != null)
                {
                    message.Visibility = Visibility.Visible;
                    message.Text = t.Result;
                }
            }));
        }

        /// <summary>The TV used last time goes first, so that it is not confused with the others.</summary>
        void AddDevice(DlnaDevice device)
        {
            if (devices.Exists(d => d.Udn == device.Udn)) return;
            bool isLast = device.Udn == CastSettings.LastDeviceUdn;
            int index = isLast ? 0 : devices.Count;
            devices.Insert(index, device);
            labels.Insert(index, isLast ? string.Format(CastStrings.LastUsed, device.Label) : device.Label);
            UpdateViews();
        }

        void UpdateViews()
        {
            searching.Visibility = isSearching ? Visibility.Visible : Visibility.Collapsed;
            message.Visibility = !isSearching && devices.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            message.Text = CastStrings.NoDevices;
            IsPrimaryButtonEnabled = !isSearching;
        }
    }
}
