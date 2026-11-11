/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Frame + state restore (WP8.1 has no SuspensionManager template in this project: the Frame state is kept in
 * LocalSettings and restored when Windows terminated the app), continuations for the file picker.
 */

using System;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.Phone.UI.Input;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Perle81.App
{
    sealed partial class App : Application
    {
        const string NavStateKey = "navState";
        public const string CrashKey = "lastCrash";

        public App()
        {
            InitializeComponent();
            UnhandledException += OnUnhandledException;
            Suspending += OnSuspending;
        }

        Frame EnsureRootFrame()
        {
            var shell = Window.Current.Content as ShellPage;
            if (shell == null)
            {
                shell = new ShellPage();
                Window.Current.Content = shell;
                HardwareButtons.BackPressed += OnBackPressed;
            }
            return shell.AppFrame;
        }

        /// <summary>
        /// Back key: the page first (full screen, overlays), then the previous page; the main page leaves the app.
        /// </summary>
        void OnBackPressed(object sender, BackPressedEventArgs e)
        {
            var shell = Window.Current.Content as ShellPage;
            var frame = shell == null ? null : shell.AppFrame;
            if (frame == null) return;

            var page = frame.Content as IBackHandler;
            if (page != null && page.HandleBack())
            {
                e.Handled = true;
                return;
            }
            if (frame.CanGoBack)
            {
                frame.GoBack();
                e.Handled = true;
            }
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            var frame = EnsureRootFrame();
            if (e.PreviousExecutionState == ApplicationExecutionState.Terminated) RestoreState(frame);
            if (frame.Content == null) frame.Navigate(typeof(MainPage), e.Arguments);
            Window.Current.Activate();
        }

        protected override void OnActivated(IActivatedEventArgs args)
        {
            var frame = EnsureRootFrame();
            if (!ContinuationManager.Handle(args, frame) && frame.Content == null) frame.Navigate(typeof(MainPage));
            Window.Current.Activate();
        }

        static void RestoreState(Frame frame)
        {
            try
            {
                var state = ApplicationData.Current.LocalSettings.Values[NavStateKey] as string;
                if (state != null) frame.SetNavigationState(state);
            }
            catch (Exception) { /* a state that no longer fits: start from the main page */ }
        }

        void OnSuspending(object sender, SuspendingEventArgs e)
        {
            var deferral = e.SuspendingOperation.GetDeferral();
            try
            {
                var shell = Window.Current.Content as ShellPage;
                var frame = shell == null ? null : shell.AppFrame;
                if (frame != null) ApplicationData.Current.LocalSettings.Values[NavStateKey] = frame.GetNavigationState();
                PlayerService.CurrentOrNull?.SavePosition();
            }
            catch (Exception) { }
            deferral.Complete();
        }

        // The only way to read crashes on the phone: keep the text, MainPage shows it once.
        void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            try { ApplicationData.Current.LocalSettings.Values[CrashKey] = e.Exception.ToString(); } catch (Exception) { }
            e.Handled = true;
        }
    }
}
