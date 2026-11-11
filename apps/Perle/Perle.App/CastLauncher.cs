/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.CastLauncher.
 */

using System;
using System.Linq;
using System.Threading.Tasks;
using Perle.Cast;
using Perle.Extractor;
using Windows.UI.Popups;
using Windows.UI.Xaml.Controls;

namespace Perle.App
{
    /// <summary>
    /// Entry point of "Cast to TV": checks that the video can be cast, then shows the list of TVs,
    /// or, while a video is already being cast, offers to play it now or add it to the TV queue.
    /// </summary>
    public static class CastLauncher
    {
        /// <returns>true if casting makes sense for this kind of stream</returns>
        public static bool IsSupported(StreamInfo info) => !info.IsLive;

        /// <summary>Shows the dialog to choose a TV, or a message if the video cannot be cast.</summary>
        public static async Task ShowAsync(Frame frame, StreamInfo info)
        {
            var item = new CastItem(info);
            if (!item.IsCastable)
            {
                CastLog.W("No castable stream for " + info.Url + ". Available: "
                    + string.Join(", ", info.Formats.Concat(info.VideoOnly).Select(f => f.QualityLabel + " " + f.MimeType + " " + f.Codecs))
                    + "; audio: " + string.Join(", ", info.AudioOnly.Select(f => f.MimeType + " " + f.Codecs)));
                await new MessageDialog(CastStrings.ErrorNoStream).ShowAsync();
                return;
            }
            if (CastService.IsActive) await ShowSessionChoiceAsync(frame, item);
            else await new CastDeviceDialog(item).ShowAndCastAsync(frame);
        }

        /// <summary>While casting, a video opened in the app goes to the TV instead of the phone player.</summary>
        /// <returns>true if the video is (now) playing on the TV, false to play it on the phone</returns>
        public static bool PlayOnTvIfCasting(StreamInfo info)
        {
            if (!CastService.IsActive || !IsSupported(info)) return false;
            // already on the TV, e.g. opened again from the casting bar
            if (CastService.State.Url == info.Url) return true;
            var item = new CastItem(info);
            if (!item.IsCastable) return false;
            CastService.Enqueue(item, playNow: true);
            return true;
        }

        public static void OpenController(Frame frame)
        {
            if (!(frame.Content is CastControllerPage)) frame.Navigate(typeof(CastControllerPage));
        }

        static async Task ShowSessionChoiceAsync(Frame frame, CastItem item)
        {
            var dialog = new ContentDialog
            {
                Title = string.Format(CastStrings.PlayingOn, CastService.State.DeviceName),
                SecondaryButtonText = "Annulla"
            };
            var list = new ListView { SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = true };
            var choices = new[] { CastStrings.PlayNow, CastStrings.AddToQueue, CastStrings.OpenController, CastStrings.OtherTv };
            list.ItemsSource = choices;
            int which = -1;
            list.ItemClick += (s, e) => { which = Array.IndexOf(choices, (string)e.ClickedItem); dialog.Hide(); };
            dialog.Content = list;
            await dialog.ShowAsync();
            switch (which)
            {
                case 0:
                    CastService.Enqueue(item, playNow: true);
                    OpenController(frame);
                    break;
                case 1:
                    CastService.Enqueue(item, playNow: false);
                    await new MessageDialog(CastStrings.AddedToQueue).ShowAsync();
                    break;
                case 2:
                    OpenController(frame);
                    break;
                case 3:
                    await new CastDeviceDialog(item).ShowAndCastAsync(frame);
                    break;
            }
        }
    }
}
