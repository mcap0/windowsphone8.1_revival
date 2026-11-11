/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The Windows side of CastService: history, settings and staying alive in the background
 * (on Android: HistoryRecordManager, preferences, foreground service, Wi-Fi and wake locks).
 */

using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using Perle.Cast;
using Perle.Cast.Dlna;
using Windows.ApplicationModel.ExtendedExecution;
using Windows.Media;
using Windows.Media.Core;
using Windows.Media.Playback;
using Windows.Networking;
using Windows.Networking.Connectivity;
using Windows.Storage;
using Windows.Storage.Streams;

namespace Perle.App
{
    /// <summary>Positions reached in the videos, shared by the phone player and the TV (like NewPipe's stream states).</summary>
    public static class Positions
    {
        const string Container = "positions";
        /// <summary>A video watched up to this many seconds from the end counts as finished.</summary>
        const long FinishedMarginSeconds = 15;

        static ApplicationDataContainer Values =>
            ApplicationData.Current.LocalSettings.CreateContainer(Container, ApplicationDataCreateDisposition.Always);

        /// <returns>where to continue the video, 0 if it was not started or finished</returns>
        public static long Get(string url, long durationSeconds)
        {
            if (url == null) return 0;
            var value = Values.Values[url];
            if (!(value is long seconds)) return 0;
            return durationSeconds > 0 && seconds >= durationSeconds - FinishedMarginSeconds ? 0 : seconds;
        }

        public static void Save(string url, long seconds)
        {
            if (url != null && seconds > 0) Values.Values[url] = seconds;
        }
    }

    public static class CastSettings
    {
        const string DirectLinkKey = "cast_direct_link";
        const string LastDeviceKey = "cast_last_device_udn";

        public static bool DirectLink
        {
            get => ApplicationData.Current.LocalSettings.Values[DirectLinkKey] as bool? ?? false;
            set => ApplicationData.Current.LocalSettings.Values[DirectLinkKey] = value;
        }

        public static string LastDeviceUdn
        {
            get => ApplicationData.Current.LocalSettings.Values[LastDeviceKey] as string;
            set => ApplicationData.Current.LocalSettings.Values[LastDeviceKey] = value;
        }
    }

    public sealed class CastHost : ICastHost
    {
        public static readonly CastHost Instance = new CastHost();

        readonly object gate = new object();
        ExtendedExecutionSession extendedExecution;
        MediaPlayer keepAlive;

        CastHost() { }

        /// <summary>Called once at startup.</summary>
        public static void Install()
        {
            CastService.Host = Instance;
            LocalNetwork.Finder = FindWifiAddress;
            CastLog.Sink = (level, message, error) => System.Diagnostics.Debug.WriteLine("Cast " + level + " " + message + (error != null ? " " + error : ""));
            CastService.StateChanged += Instance.UpdateMediaControls;
        }

        public long SavedPositionSeconds(CastItem item) => Positions.Get(item.Info.Url, item.DurationSeconds);

        public void SaveProgress(CastItem item, long positionSeconds) => Positions.Save(item.Info.Url, positionSeconds);

        public void MarkAsViewed(CastItem item) => AppState.Repository.AddToHistoryAsync(item.Info.Url).GetAwaiter().GetResult();

        public bool DirectLink => CastSettings.DirectLink;

        /// <summary>
        /// Windows 10 Mobile suspends an app in the background or with the screen off, which
        /// would stop the proxy the TV downloads from. An app playing audio is not suspended, so
        /// a silent sound is played in a loop while casting; its media controls (lock screen,
        /// volume popup) control the TV, like the notification on Android. Extended execution is
        /// asked too, for the time the app is minimized with the screen on.
        /// </summary>
        public void SessionStarted()
        {
            lock (gate)
            {
                if (keepAlive == null)
                {
                    keepAlive = new MediaPlayer { IsLoopingEnabled = true, Volume = 0 };
                    keepAlive.CommandManager.IsEnabled = false;
                    var controls = keepAlive.SystemMediaTransportControls;
                    controls.IsEnabled = true;
                    controls.IsPlayEnabled = true;
                    controls.IsPauseEnabled = true;
                    controls.IsStopEnabled = true;
                    controls.IsNextEnabled = false;
                    controls.ButtonPressed += OnMediaButton;
                    keepAlive.Source = MediaSource.CreateFromStream(SilentWav(), "audio/wav");
                    keepAlive.Play();
                }
            }
            RequestExtendedExecution();
        }

        public void SessionEnded()
        {
            lock (gate)
            {
                if (keepAlive != null)
                {
                    keepAlive.SystemMediaTransportControls.ButtonPressed -= OnMediaButton;
                    keepAlive.SystemMediaTransportControls.PlaybackStatus = MediaPlaybackStatus.Closed;
                    keepAlive.SystemMediaTransportControls.DisplayUpdater.ClearAll();
                    keepAlive.Pause();
                    keepAlive.Dispose();
                    keepAlive = null;
                }
                extendedExecution?.Dispose();
                extendedExecution = null;
            }
        }

        async void RequestExtendedExecution()
        {
            ExtendedExecutionSession session;
            lock (gate)
            {
                if (extendedExecution != null) return;
                session = extendedExecution = new ExtendedExecutionSession { Reason = ExtendedExecutionReason.Unspecified, Description = CastStrings.CastToTv };
            }
            session.Revoked += (s, e) =>
            {
                CastLog.W("Extended execution revoked: " + e.Reason);
                lock (gate) { if (extendedExecution == session) extendedExecution = null; }
                session.Dispose();
            };
            try
            {
                var result = await session.RequestExtensionAsync();
                CastLog.D("Extended execution: " + result);
            }
            catch (Exception e)
            {
                CastLog.W("Extended execution not available", e);
            }
        }

        void OnMediaButton(SystemMediaTransportControls sender, SystemMediaTransportControlsButtonPressedEventArgs args)
        {
            switch (args.Button)
            {
                case SystemMediaTransportControlsButton.Play:
                case SystemMediaTransportControlsButton.Pause:
                    CastService.PlayPause();
                    break;
                case SystemMediaTransportControlsButton.Stop:
                    CastService.Stop();
                    break;
                case SystemMediaTransportControlsButton.Next:
                    CastService.Next();
                    break;
            }
        }

        string shownTitle;
        CastStatus? shownStatus;
        bool shownHasNext;

        /// <summary>Like CastService.updateNotification: only when the title, the status or the queue changes.</summary>
        void UpdateMediaControls(CastState state)
        {
            MediaPlayer player;
            lock (gate) player = keepAlive;
            if (player == null || !state.Active) return;
            bool hasNext = state.QueueIndex + 1 < state.Queue.Count;
            if (state.Title == shownTitle && state.Status == shownStatus && hasNext == shownHasNext) return;
            shownTitle = state.Title;
            shownStatus = state.Status;
            shownHasNext = hasNext;
            try
            {
                var controls = player.SystemMediaTransportControls;
                controls.IsNextEnabled = hasNext;
                controls.PlaybackStatus = state.Status == CastStatus.Playing ? MediaPlaybackStatus.Playing
                    : state.Status == CastStatus.Paused ? MediaPlaybackStatus.Paused
                    : state.Status == CastStatus.Stopped ? MediaPlaybackStatus.Stopped
                    : MediaPlaybackStatus.Changing;
                var display = controls.DisplayUpdater;
                display.Type = MediaPlaybackType.Video;
                display.VideoProperties.Title = state.Title ?? "";
                display.VideoProperties.Subtitle = string.Format(CastStrings.NotificationText, CastStatuses.Text(state.Status), state.DeviceName);
                if (state.ThumbnailUrl != null)
                    display.Thumbnail = RandomAccessStreamReference.CreateFromUri(new Uri(state.ThumbnailUrl));
                display.Update();
            }
            catch (Exception e)
            {
                CastLog.W("Could not update the media controls: " + e.Message);
            }
        }

        /// <returns>one second of 8 kHz mono silence</returns>
        static IRandomAccessStream SilentWav()
        {
            const int sampleRate = 8000;
            var data = new byte[sampleRate]; // 8 bit unsigned: 128 is silence
            for (int i = 0; i < data.Length; i++) data[i] = 128;
            var wav = new MemoryStream();
            var w = new BinaryWriter(wav);
            w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + data.Length); w.Write(Encoding.ASCII.GetBytes("WAVE"));
            w.Write(Encoding.ASCII.GetBytes("fmt ")); w.Write(16); w.Write((short)1); w.Write((short)1);
            w.Write(sampleRate); w.Write(sampleRate); w.Write((short)1); w.Write((short)8);
            w.Write(Encoding.ASCII.GetBytes("data")); w.Write(data.Length); w.Write(data);
            w.Flush();
            wav.Position = 0;
            return wav.AsRandomAccessStream();
        }

        /// <summary>
        /// The IPv4 address of the Wi-Fi adapter, from the Windows networking API (more reliable on
        /// Windows 10 Mobile than System.Net.NetworkInformation), null if the phone is not on Wi-Fi.
        /// </summary>
        static LocalAddress FindWifiAddress()
        {
            const uint Wifi = 71, Ethernet = 6;
            try
            {
                var candidates = NetworkInformation.GetHostNames()
                    .Where(h => h.Type == HostNameType.Ipv4 && h.IPInformation?.NetworkAdapter != null)
                    .Select(h => new { Host = h, Address = IPAddress.Parse(h.CanonicalName), h.IPInformation.NetworkAdapter.IanaInterfaceType })
                    // not the mobile data connection, whose addresses can be private too (carrier NAT)
                    .Where(c => LanHttp.IsLocalAddress(c.Address) && (c.IanaInterfaceType == Wifi || c.IanaInterfaceType == Ethernet))
                    .OrderByDescending(c => c.IanaInterfaceType == Wifi)
                    .ToList();
                var best = candidates.FirstOrDefault();
                if (best != null) return new LocalAddress(best.IanaInterfaceType == Wifi ? "wlan" : "if" + best.IanaInterfaceType, best.Address);
            }
            catch (Exception e)
            {
                CastLog.W("Could not list the network adapters", e);
            }
            return null;
        }
    }
}
