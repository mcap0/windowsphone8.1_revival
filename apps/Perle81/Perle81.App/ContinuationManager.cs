/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Routes the continuation of FileOpenPicker.PickSingleFileAndContinue (WP8.1) to the import page.
 */

using Windows.ApplicationModel.Activation;
using Windows.Storage.Pickers;
using Windows.UI.Xaml.Controls;

namespace Perle81.App
{
    public static class ContinuationManager
    {
        /// <returns>true when the activation was a continuation this app handles</returns>
        public static bool Handle(IActivatedEventArgs args, Frame frame)
        {
            var picked = args as FileOpenPickerContinuationEventArgs;
            if (picked == null) return false;
            AppState.PendingImport = picked.Files.Count > 0 ? picked.Files[0] : null;
            frame.Navigate(typeof(ImportPage));
            return true;
        }
    }
}
