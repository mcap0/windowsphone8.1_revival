/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.CastService. The Android foreground service, notification and
 * locks are replaced by ICastHost, implemented by the app (extended execution, media controls).
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Threading;
using Perle.Cast.Dlna;
using Perle.Cast.Proxy;
using Perle.Extractor;

namespace Perle.Cast
{
    /// <summary>What the TV is doing, as shown in the controller and in the media controls.</summary>
    public enum CastStatus { Connecting, Buffering, Playing, Paused, Stopped, Error }

    public static class CastStatuses
    {
        public static string Text(CastStatus status)
        {
            switch (status)
            {
                case CastStatus.Connecting: return CastStrings.StatusConnecting;
                case CastStatus.Buffering: return CastStrings.StatusBuffering;
                case CastStatus.Playing: return CastStrings.StatusPlaying;
                case CastStatus.Paused: return CastStrings.StatusPaused;
                case CastStatus.Stopped: return CastStrings.StatusStopped;
                default: return CastStrings.StatusError;
            }
        }
    }

    /// <summary>A video of the queue, as shown in the controller.</summary>
    public sealed class CastQueueEntry
    {
        public string Title { get; }
        public string Uploader { get; }
        public long DurationSeconds { get; }
        public CastQueueEntry(string title, string uploader, long durationSeconds) { Title = title; Uploader = uploader; DurationSeconds = durationSeconds; }
    }

    /// <summary>
    /// State of the current cast session, published by CastService.StateChanged. Never modified
    /// once published: changes are made on a Copy().
    /// </summary>
    public sealed class CastState
    {
        /// <summary>false when there is no session (the controller then closes itself)</summary>
        public bool Active { get; set; }
        public string DeviceName { get; set; } = "";
        /// <summary>URL of the video being cast</summary>
        public string Url { get; set; } = "";
        public string Title { get; set; } = "";
        public string Uploader { get; set; } = "";
        public string ThumbnailUrl { get; set; }
        public CastStatus Status { get; set; } = CastStatus.Connecting;
        public long PositionSeconds { get; set; } = -1;
        public long DurationSeconds { get; set; } = -1;
        public bool HasVolumeControl { get; set; }
        public int Volume { get; set; } = -1;
        /// <summary>a user readable error, null if everything is fine</summary>
        public string Error { get; set; }
        public List<CastQueueEntry> Queue { get; set; } = new List<CastQueueEntry>();
        public int QueueIndex { get; set; }
        /// <summary>the resolutions that can be chosen, highest first</summary>
        public List<int> Qualities { get; set; } = new List<int>();
        /// <summary>the resolution being cast, 0 if unknown</summary>
        public int Quality { get; set; }
        /// <summary>true if the user did not choose a resolution</summary>
        public bool AutoQuality { get; set; } = true;
        /// <summary>the names of the audio tracks that can be chosen, empty if only one</summary>
        public List<string> AudioTracks { get; set; } = new List<string>();
        /// <summary>the index in AudioTracks of the track being cast</summary>
        public int AudioTrack { get; set; }
        /// <summary>the names of the subtitles that can be chosen</summary>
        public List<string> Subtitles { get; set; } = new List<string>();
        /// <summary>the index in Subtitles of the subtitles sent to the TV, -1 for none</summary>
        public int Subtitle { get; set; } = -1;

        public CastState Copy() => (CastState)MemberwiseClone();
    }

    /// <summary>What the app provides to the cast session.</summary>
    public interface ICastHost
    {
        /// <returns>the position saved in the history for the video, in seconds, or 0</returns>
        long SavedPositionSeconds(CastItem item);

        /// <summary>Saves the position reached on the TV, like the local player does.</summary>
        void SaveProgress(CastItem item, long positionSeconds);

        /// <summary>Adds the video to the watch history, as if it was played in the app.</summary>
        void MarkAsViewed(CastItem item);

        /// <summary>true to give the TV the direct link instead of the proxy URL (settings).</summary>
        bool DirectLink { get; }

        /// <summary>Keeps the app running in the background while a session is active (Android: foreground service and locks).</summary>
        void SessionStarted();

        void SessionEnded();
    }

    /// <summary>
    /// Owns a cast session: it runs the local proxy, sends the videos of the queue to the TV one
    /// after the other, polls the TV every second for its state and handles the commands from the
    /// controller and the media controls.
    ///
    /// The position reached is saved in the history like for the local player, so that the video
    /// continues from there, on the TV or on the phone.
    ///
    /// All the network work happens on a single worker thread, so commands are executed in order.
    /// </summary>
    public sealed class CastService
    {
        const int ErrorTransitionNotAvailable = 701;
        const int PlayAttempts = 4;
        const int StopWaitPolls = 10;
        const int StopWaitMillis = 300;
        const int StoppedPollsBeforeEnd = 3;
        const long StartTimeoutMillis = 25000;
        const long StoppedGraceMillis = 5000;
        const int FailedPollsBeforeError = 8;
        const int SaveEveryPolls = 5;
        const int EndMarginSeconds = 15;
        const int VolumeStep = 2;
        const int MaxVolume = 100;

        static volatile CastState currentState = new CastState();
        static CastService instance;
        static readonly object instanceGate = new object();

        /// <summary>Called on the worker thread with every new state; the UI dispatches it to its thread.</summary>
        public static event Action<CastState> StateChanged;

        /// <summary>The state of the current session.</summary>
        public static CastState State => currentState;

        /// <summary>true while a video is being cast</summary>
        public static bool IsActive => currentState.Active;

        /// <summary>
        /// Pauses the player of the phone so that the video does not play twice, set by the app.
        /// Gets the URL of the video and returns where the phone player was in it, in milliseconds,
        /// or -1 if it was not playing it.
        /// </summary>
        public static Func<string, long> PauseForCasting;

        /// <summary>The app's implementation of ICastHost, set at startup.</summary>
        public static ICastHost Host;

        /// <summary>Factories of the proxy upstreams, replaceable in tests.</summary>
        public static Func<IUpstream> UpstreamFactory = () => new HttpUpstream();
        public static Func<IUpstream> DashUpstreamFactory = () => new HttpUpstream(useRangeParameter: true);

        /// <summary>Downloads the subtitles, replaceable in tests.</summary>
        public static Func<string, string> DownloadText = url =>
        {
            using (var client = new HttpClient { Timeout = TimeSpan.FromSeconds(20) })
            {
                client.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent", Innertube.BrowserUserAgent);
                return client.GetStringAsync(url).GetAwaiter().GetResult();
            }
        };

        readonly SerialWorker worker = new SerialWorker("CastService");

        // only accessed on the worker thread
        DlnaDevice device;
        DlnaClient client;
        CastProxyServer proxy;
        string sinkProtocolInfo;
        bool sinkProtocolInfoLoaded;
        IDisposable polling;
        bool hasPlayed;

        // when the TV was asked to play, to notice a video it cannot play
        long playRequestedAt;
        TransportState? lastLoggedTransport;

        // shown while the video plays at a lower resolution than wanted
        string fallbackNotice;
        int stoppedPolls;
        int failedPolls;
        int polls;
        int sessionId;

        readonly List<CastItem> queue = new List<CastItem>();
        int queueIndex;

        // the choices of the user, kept for the next videos of the queue
        int? wantedHeight;
        string wantedAudioTrackId;
        string wantedSubtitles;

        // what is being cast
        CastChoice currentChoice;
        SubtitleTrack currentSubtitles;

        // set for live MPEG-TS sessions, which seek by restarting the stream at another time
        CastMedia liveMedia;
        CastDash liveDash;
        TsFormat liveFormat;

        // the subtitles sent to the TV, as SRT
        string currentSrt;
        long positionOffset;

        // position to seek to once the TV plays a video which does not start at the beginning
        long pendingSeek;

        // last position seen while playing, saved in the history
        long lastPosition = -1;

        CastService() { }

        static CastService Instance
        {
            get { lock (instanceGate) return instance ?? (instance = new CastService()); }
        }

        static long Now() => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

        // /////////////////////////////////////////////////////////////////////////
        // Commands (any thread)
        // /////////////////////////////////////////////////////////////////////////

        /// <summary>Starts casting item to device, replacing the current session if any.</summary>
        public static void Start(DlnaDevice device, CastItem item)
        {
            HandOffFromPhone(item);
            // published right away, so that the controller opened next does not see the old state
            var service = Instance;
            service.Publish(InitialState(device, item));
            Host?.SessionStarted();
            service.worker.Execute(() => service.StartSession(device, item));
        }

        /// <summary>Adds item to the queue of the current session.</summary>
        /// <param name="playNow">true to play it right away, false to play it after the others</param>
        public static void Enqueue(CastItem item, bool playNow)
        {
            if (playNow && currentState.Active) HandOffFromPhone(item);
            Command(s => s.EnqueueItem(item, playNow));
        }

        public static void PlayPause() => Command(s => s.DoPlayPause());

        public static void Stop() => Command(s => s.StopSession(stopTv: true));

        public static void Next() => Command(s => s.DoPlayIndex(s.queueIndex + 1));

        public static void PlayIndex(int index) => Command(s => s.DoPlayIndex(index));

        public static void Remove(int index) => Command(s => s.DoRemove(index));

        /// <param name="height">the resolution, or 0 for automatic</param>
        public static void SetQuality(int height) => Command(s => s.DoSetQuality(height > 0 ? height : (int?)null));

        public static void SetAudioTrack(int index) => Command(s => s.DoSetAudioTrack(index));

        /// <param name="index">the index in CastState.Subtitles, -1 for none</param>
        public static void SetSubtitles(int index) => Command(s => s.DoSetSubtitles(index));

        public static void VolumeUp() => Command(s => s.ChangeVolume(+VolumeStep));

        public static void VolumeDown() => Command(s => s.ChangeVolume(-VolumeStep));

        public static void Seek(long positionSeconds) => Command(s => s.DoSeek(positionSeconds));

        static void Command(Action<CastService> action)
        {
            if (!currentState.Active) return;
            var service = Instance;
            service.worker.Execute(() => action(service));
        }

        /// <summary>
        /// Pauses the player of the phone, so that the video does not play twice, and continues
        /// on the TV from where the phone was if it was playing the same video.
        /// </summary>
        static void HandOffFromPhone(CastItem item)
        {
            try
            {
                long position = PauseForCasting?.Invoke(item.Info.Url) ?? -1;
                if (position >= 0)
                {
                    item.HandOffSeconds = position / 1000;
                    CastLog.D("Continuing on the TV from the phone player at " + position / 1000 + "s");
                }
            }
            catch (Exception e)
            {
                CastLog.W("Could not pause the phone player: " + e.Message);
            }
        }

        static CastState InitialState(DlnaDevice device, CastItem item) => new CastState
        {
            Active = true,
            DeviceName = device.FriendlyName,
            Url = item.Info.Url ?? "",
            Title = item.Title,
            Uploader = item.Uploader,
            ThumbnailUrl = item.Info.ThumbnailUrl,
            DurationSeconds = item.DurationSeconds,
            Queue = new List<CastQueueEntry> { new CastQueueEntry(item.Title, item.Uploader, item.DurationSeconds) }
        };

        // /////////////////////////////////////////////////////////////////////////
        // Session (worker thread)
        // /////////////////////////////////////////////////////////////////////////

        void StartSession(DlnaDevice device, CastItem item)
        {
            EndSession(keepState: true);
            int session = ++sessionId;
            CastLog.D("Starting session #" + session + " on \"" + device.FriendlyName + "\" for \"" + item.Title + "\" (Perle for Windows 10 Mobile)");

            this.device = device;
            client = new DlnaClient(device);
            sinkProtocolInfo = null;
            sinkProtocolInfoLoaded = false;
            queue.Clear();
            queue.Add(item);
            queueIndex = 0;

            try
            {
                var local = LocalNetwork.Find() ?? throw new InvalidOperationException("No local network address");
                // the separate DASH streams are requested like NewPipe's player does, see YoutubeRange
                proxy = new CastProxyServer(UpstreamFactory(), local.Address, DashUpstreamFactory());
                proxy.Start();
            }
            catch (Exception e)
            {
                CastLog.E("Could not start the local proxy", e);
                Fail(CastStrings.ErrorNoWifi);
                return;
            }

            if (!PlayCurrent(startSeconds: null)) return;

            var dlna = client;
            if (dlna == null) return;
            if (dlna.HasVolumeControl)
            {
                int volume;
                try { volume = dlna.GetVolume(); } catch (Exception) { volume = -1; }
                Publish(With(s => { s.HasVolumeControl = volume >= 0; s.Volume = volume; }));
            }
            polling = worker.ScheduleWithFixedDelay(() => Poll(session), TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
        }

        /// <summary>Sends the current video of the queue to the TV.</summary>
        /// <param name="startSeconds">where to start, null to continue from the position saved in the history</param>
        /// <param name="isNewVideo">false when the same video is sent again with other streams</param>
        /// <returns>false if the session failed</returns>
        bool PlayCurrent(long? startSeconds, bool isNewVideo = true)
        {
            var dlna = client;
            var server = proxy;
            if (dlna == null || server == null) return false;
            if (queueIndex < 0 || queueIndex >= queue.Count) return false;
            var item = queue[queueIndex];
            hasPlayed = false;
            stoppedPolls = 0;
            failedPolls = 0;
            pendingSeek = 0;
            liveMedia = null;
            liveDash = null;
            liveFormat = null;
            currentSrt = null;
            positionOffset = 0;

            var handOff = item.HandOffSeconds;
            item.HandOffSeconds = null;
            long start = Math.Max(0, startSeconds ?? handOff ?? SavedPosition(item));
            lastPosition = start;
            if (isNewVideo)
            {
                fallbackNotice = null;
                MarkAsViewed(item);
            }

            // the iOS URLs of the Android fork (IosStreamUrls) are not needed: the VISIONOS URLs
            // used here can be read to the end with any range
            var choice = item.Choose(item.FallbackHeight ?? wantedHeight, wantedAudioTrackId);
            if (choice == null)
            {
                Fail(CastStrings.ErrorNoStream);
                return false;
            }
            currentChoice = choice;
            var media = item.Media(choice);
            currentSubtitles = item.FindSubtitles(wantedSubtitles);
            currentSrt = currentSubtitles != null ? LoadSrt(currentSubtitles) : null;
            CastLog.D("Casting \"" + item.Title + "\" (" + (queueIndex + 1) + "/" + queue.Count + ") from " + start + "s: "
                + (choice.Single != null ? choice.Single.Height + "p MP4" : choice.Dash.Video.Height + "p + " + choice.Dash.Audio.Codecs + " muxed")
                + (currentSubtitles != null ? ", subtitles " + currentSubtitles.LanguageTag : ""));
            var state = StateForCurrent(item);
            state.Status = CastStatus.Connecting;
            state.PositionSeconds = start;
            state.Error = null;
            Publish(state);

            var castMedia = PrepareMedia(media, dlna, server, start);
            try
            {
                StopIfBusy(dlna);
                SetUriWithRetries(dlna, castMedia);
                PlayWithRetries(dlna);
            }
            catch (UpnpException e)
            {
                var error = e.ErrorCode + " " + e.ErrorDescription;
                // still busy after the retries: the TV is usually off, or showing another app
                Fail(e.ErrorCode == ErrorTransitionNotAvailable ? CastStrings.ErrorTvBusy
                    : media.Dash != null ? string.Format(CastStrings.ErrorLiveRefused, error)
                    : string.Format(CastStrings.ErrorTv, error));
                return false;
            }
            catch (Exception e)
            {
                CastLog.E("Could not start playback", e);
                Fail(CastStrings.ErrorUnreachable);
                return false;
            }
            playRequestedAt = Now();
            lastLoggedTransport = null;
            Publish(With(s => s.Status = CastStatus.Buffering));
            return true;
        }

        /// <summary>
        /// Some TVs refuse a new URI while they are playing something else, or accept it and then
        /// never start it: stop the TV and wait until it says it stopped.
        /// </summary>
        void StopIfBusy(DlnaClient dlna)
        {
            var state = TryGet(() => (TransportState?)dlna.GetTransportInfo(), null);
            if (state != TransportState.Playing && state != TransportState.Paused && state != TransportState.Transitioning) return;
            long started = Now();
            Try(dlna.Stop);
            var now = state;
            for (int i = 0; i < StopWaitPolls; i++)
            {
                now = TryGet(() => (TransportState?)dlna.GetTransportInfo(), null);
                if (now == TransportState.Stopped || now == TransportState.NoMedia)
                {
                    CastLog.D("The TV stopped (" + state + " -> " + now + ") in " + (Now() - started) + " ms");
                    return;
                }
                Thread.Sleep(StopWaitMillis);
            }
            CastLog.W("The TV is still " + now + " " + (Now() - started) + " ms after Stop");
        }

        /// <summary>
        /// While it is still switching from the previous video, the TV answers SetAVTransportURI
        /// with 701 "Transition not available": stop it and try again.
        /// </summary>
        void SetUriWithRetries(DlnaClient dlna, CastMedia media)
        {
            int attempt = 0;
            while (true)
            {
                try
                {
                    dlna.SetAvTransportUri(media);
                    return;
                }
                catch (UpnpException e) when (e.ErrorCode == ErrorTransitionNotAvailable && ++attempt < PlayAttempts)
                {
                    CastLog.D("The TV is busy, stopping it and retrying in 1 second");
                    Try(dlna.Stop);
                    Thread.Sleep(1000);
                }
            }
        }

        /// <summary>
        /// The TV did not start the video: it often cannot decode some streams (e.g. high H.264
        /// levels), so the video is sent again at the next lower resolution.
        /// </summary>
        /// <returns>false if there is no lower resolution</returns>
        bool TryLowerQuality(string reason)
        {
            if (queueIndex < 0 || queueIndex >= queue.Count || currentChoice == null) return false;
            var item = queue[queueIndex];
            int current = currentChoice.Height;
            var lowers = item.Qualities.Where(q => q < current).ToList();
            if (lowers.Count == 0) return false;
            int lower = lowers.Max();
            CastLog.W("The TV did not play " + current + "p (" + reason + "), trying " + lower + "p");
            item.FallbackHeight = lower;
            long start = Math.Max(0, lastPosition);
            if (PlayCurrent(startSeconds: start, isNewVideo: false))
            {
                fallbackNotice = string.Format(CastStrings.QualityFallback, current + "p", lower + "p");
                Publish(With(s => s.Error = fallbackNotice));
            }
            return true;
        }

        /// <returns>what to give to the TV: the URL through the local proxy, or the direct link if the
        /// user chose so in the settings. Separate video and audio streams always go through the
        /// proxy, which muxes them into MPEG-TS.</returns>
        CastMedia PrepareMedia(CastMedia media, DlnaClient dlna, CastProxyServer server, long startSeconds)
        {
            var dash = media.Dash;
            if (dash != null)
            {
                var format = TsFormat.Choose(SinkProtocolInfo(dlna));
                liveDash = dash;
                liveFormat = format;
                liveMedia = media.Copy();
                liveMedia.MimeType = format.MimeType;
                liveMedia.DlnaFeatures = format.DlnaFeatures;
                return LiveMediaAt(startSeconds, server);
            }

            pendingSeek = startSeconds;
            var subtitleUrl = currentSrt != null ? RegisterSrt(currentSrt, 0, server) : null;
            if (Host?.DirectLink == true && subtitleUrl == null)
            {
                CastLog.D("Sending the direct link to the TV (proxy disabled in the settings)");
                return media;
            }
            var url = server.Register(media.Url, media.MimeType, subtitleUrl);
            CastLog.D("Proxy URL for the TV: " + url);
            var result = media.Copy();
            result.Url = url;
            result.SubtitleUrl = subtitleUrl;
            return result;
        }

        string SinkProtocolInfo(DlnaClient dlna)
        {
            if (!sinkProtocolInfoLoaded)
            {
                sinkProtocolInfoLoaded = true;
                try { sinkProtocolInfo = dlna.GetSinkProtocolInfo(); }
                catch (Exception e)
                {
                    CastLog.W("Could not read the formats supported by the TV: " + e.Message);
                    sinkProtocolInfo = null;
                }
                // only the MIME types, with how many profiles each: the whole list is very long
                var formats = TsFormat.Parse(sinkProtocolInfo).GroupBy(p => p.Key, p => p.Value);
                CastLog.D("Video formats declared by the TV: "
                    + string.Join(", ", formats.Select(g => g.Key + " (" + g.Count(v => v != null) + " profiles)")));
            }
            return sinkProtocolInfo;
        }

        /// <returns>the live MPEG-TS stream starting at startSeconds. Its timestamps start at 0, so
        /// the subtitles are shifted to match.</returns>
        CastMedia LiveMediaAt(long startSeconds, CastProxyServer server)
        {
            var live = liveMedia;
            var subtitleUrl = currentSrt != null ? RegisterSrt(currentSrt, startSeconds, server) : null;
            var url = server.RegisterMux(liveDash, liveFormat, subtitleUrl);
            CastLog.D("Live MPEG-TS URL for the TV (" + live.MimeType + ", " + (liveFormat.Profile ?? "no profile") + "): " + url);
            positionOffset = startSeconds;
            var result = live.Copy();
            result.Url = startSeconds > 0 ? url + "?t=" + startSeconds : url;
            result.SubtitleUrl = subtitleUrl;
            return result;
        }

        /// <summary>Downloads the subtitles and converts them to SRT.</summary>
        string LoadSrt(SubtitleTrack subtitles)
        {
            try
            {
                var text = DownloadText(subtitles.UrlFor(CastItem.SubtitleFormat));
                return SrtConverter.Convert(text, CastItem.SubtitleFormat)
                    ?? throw new ArgumentException("Unsupported subtitle format " + CastItem.SubtitleFormat);
            }
            catch (Exception e)
            {
                CastLog.E("Could not load the subtitles", e);
                return null;
            }
        }

        /// <summary>Serves the subtitles to the TV, starting at startSeconds.</summary>
        string RegisterSrt(string srt, long startSeconds, CastProxyServer server)
        {
            var shifted = startSeconds > 0 ? SrtConverter.Shift(srt, startSeconds * 1000) : srt;
            var url = server.RegisterContent(Encoding.UTF8.GetBytes(shifted), DidlLite.SrtMimeType, "srt");
            CastLog.D("Subtitles for the TV: " + url);
            return url;
        }

        /// <summary>Right after SetAVTransportURI some TVs answer Play with 701 until they are ready.</summary>
        void PlayWithRetries(DlnaClient dlna)
        {
            int attempt = 0;
            while (true)
            {
                try
                {
                    dlna.Play();
                    return;
                }
                catch (UpnpException e) when (e.ErrorCode == ErrorTransitionNotAvailable && ++attempt < PlayAttempts)
                {
                    CastLog.D("Play not possible yet, retrying in 1 second");
                    Thread.Sleep(1000);
                }
            }
        }

        void Poll(int session)
        {
            var dlna = client;
            if (dlna == null || session != sessionId) return;
            try
            {
                var transport = dlna.GetTransportInfo();
                var position = dlna.GetPositionInfo();
                failedPolls = 0;

                CastStatus status;
                switch (transport)
                {
                    case TransportState.Playing: status = CastStatus.Playing; break;
                    case TransportState.Paused: status = CastStatus.Paused; break;
                    case TransportState.Transitioning: status = CastStatus.Buffering; break;
                    case TransportState.Stopped:
                    case TransportState.NoMedia: status = CastStatus.Stopped; break;
                    default: status = currentState.Status; break;
                }
                if (transport != lastLoggedTransport)
                {
                    CastLog.D("TV state: " + transport + ", position " + position.PositionSeconds + "s");
                    lastLoggedTransport = transport;
                }
                if (status == CastStatus.Playing || status == CastStatus.Paused) hasPlayed = true;

                if (!hasPlayed)
                {
                    long waited = Now() - playRequestedAt;
                    stoppedPolls = status == CastStatus.Stopped && waited > StoppedGraceMillis ? stoppedPolls + 1 : 0;
                    string reason = stoppedPolls >= StoppedPollsBeforeEnd ? "it stopped"
                        : waited > StartTimeoutMillis ? "nothing after " + waited / 1000 + "s"
                        : null;
                    if (reason != null)
                    {
                        stoppedPolls = 0;
                        if (!TryLowerQuality(reason)) Fail(CastStrings.ErrorCannotPlay);
                        return;
                    }
                }

                if (status == CastStatus.Playing && pendingSeek > 0)
                {
                    // TVs ignore seeks before they play, so the saved position is reached now
                    long target = pendingSeek;
                    pendingSeek = 0;
                    CastLog.D("Continuing from " + target + "s");
                    try { dlna.Seek(target); }
                    catch (Exception e) { CastLog.W("Could not seek to " + target + "s: " + e.Message); }
                    Publish(With(s => s.PositionSeconds = target));
                    return;
                }

                long positionSeconds = position.PositionSeconds >= 0 ? position.PositionSeconds + positionOffset : -1;
                if ((status == CastStatus.Playing || status == CastStatus.Paused) && positionSeconds >= 0 && pendingSeek == 0)
                    lastPosition = positionSeconds;
                Publish(With(s =>
                {
                    s.Status = !hasPlayed && status == CastStatus.Stopped ? CastStatus.Buffering : status;
                    s.PositionSeconds = pendingSeek > 0 ? s.PositionSeconds : positionSeconds;
                    s.DurationSeconds = liveMedia != null ? s.DurationSeconds
                        : position.DurationSeconds > 0 ? position.DurationSeconds : s.DurationSeconds;
                    s.Error = fallbackNotice;
                }));

                if (++polls % SaveEveryPolls == 0 && status == CastStatus.Playing) SaveProgress();

                // the video ended, or the TV was used for something else
                if (hasPlayed) stoppedPolls = status == CastStatus.Stopped ? stoppedPolls + 1 : 0;
                if (hasPlayed && stoppedPolls >= StoppedPollsBeforeEnd) OnTvStopped();
            }
            catch (Exception)
            {
                failedPolls++;
                if (failedPolls >= FailedPollsBeforeError) Fail(CastStrings.ErrorUnreachable);
            }
        }

        /// <summary>The TV stopped by itself: at the end of the video the queue continues.</summary>
        void OnTvStopped()
        {
            var item = queueIndex >= 0 && queueIndex < queue.Count ? queue[queueIndex] : null;
            long duration = currentState.DurationSeconds > 0 ? currentState.DurationSeconds : item?.DurationSeconds ?? -1;
            bool ended = duration <= 0 || lastPosition >= duration - EndMarginSeconds;
            if (ended && item != null && duration > 0) lastPosition = duration;
            if (ended && queueIndex + 1 < queue.Count)
            {
                CastLog.D("The video ended, playing the next one of the queue");
                SaveProgress();
                queueIndex++;
                PlayCurrent(startSeconds: null);
                return;
            }
            CastLog.D("The TV stopped playing, ending the session");
            StopSession(stopTv: false);
        }

        void EnqueueItem(CastItem item, bool playNow)
        {
            if (client == null) return;
            if (playNow)
            {
                SaveProgress();
                queue.Insert(queueIndex + 1, item);
                queueIndex++;
                PlayCurrent(startSeconds: null);
            }
            else
            {
                queue.Add(item);
                CastLog.D("Added \"" + item.Title + "\" to the queue (" + queue.Count + " videos)");
                Publish(With(s => s.Queue = QueueEntries()));
            }
        }

        void DoPlayIndex(int index)
        {
            if (client == null || index < 0 || index >= queue.Count || index == queueIndex) return;
            SaveProgress();
            queueIndex = index;
            PlayCurrent(startSeconds: null);
        }

        void DoRemove(int index)
        {
            if (index < 0 || index >= queue.Count || index == queueIndex) return;
            queue.RemoveAt(index);
            if (index < queueIndex) queueIndex--;
            Publish(With(s => { s.Queue = QueueEntries(); s.QueueIndex = queueIndex; }));
        }

        void DoSetQuality(int? height)
        {
            if (client == null) return;
            wantedHeight = height;
            if (queueIndex >= 0 && queueIndex < queue.Count) queue[queueIndex].FallbackHeight = null;
            fallbackNotice = null;
            RestartCurrent();
        }

        void DoSetAudioTrack(int index)
        {
            if (queueIndex < 0 || queueIndex >= queue.Count) return;
            var item = queue[queueIndex];
            if (index < 0 || index >= item.AudioTrackIds.Count) return;
            // the first track is the original one, which the progressive MP4 streams have too
            wantedAudioTrackId = index == 0 ? null : item.AudioTrackIds[index];
            RestartCurrent();
        }

        void DoSetSubtitles(int index)
        {
            if (queueIndex < 0 || queueIndex >= queue.Count) return;
            var item = queue[queueIndex];
            wantedSubtitles = index >= 0 && index < item.Subtitles.Count ? CastItem.SubtitleKey(item.Subtitles[index]) : null;
            RestartCurrent();
        }

        /// <summary>Sends the current video again with the new choices, from the same position.</summary>
        void RestartCurrent()
        {
            if (client == null) return;
            long position = Math.Max(0, lastPosition);
            SaveProgress();
            PlayCurrent(startSeconds: position, isNewVideo: false);
        }

        void DoPlayPause()
        {
            var dlna = client;
            if (dlna == null) return;
            try
            {
                if (currentState.Status == CastStatus.Playing)
                {
                    dlna.Pause();
                    Publish(With(s => s.Status = CastStatus.Paused));
                    SaveProgress();
                }
                else
                {
                    dlna.Play();
                    Publish(With(s => s.Status = CastStatus.Playing));
                }
            }
            catch (Exception e)
            {
                ShowCommandError(e);
            }
        }

        void DoSeek(long positionSeconds)
        {
            var dlna = client;
            if (dlna == null || positionSeconds < 0) return;
            pendingSeek = 0;
            var server = proxy;
            if (server == null) return;
            if (liveMedia != null)
            {
                // a live stream cannot be seeked: the TV gets a new one starting at that time
                try
                {
                    StopIfBusy(dlna);
                    SetUriWithRetries(dlna, LiveMediaAt(positionSeconds, server));
                    PlayWithRetries(dlna);
                    playRequestedAt = Now();
                    lastPosition = positionSeconds;
                    hasPlayed = false;
                    stoppedPolls = 0;
                    Publish(With(s => { s.PositionSeconds = positionSeconds; s.Status = CastStatus.Buffering; }));
                }
                catch (Exception e)
                {
                    ShowCommandError(e);
                }
                return;
            }
            try
            {
                dlna.Seek(positionSeconds);
                lastPosition = positionSeconds;
                Publish(With(s => s.PositionSeconds = positionSeconds));
            }
            catch (Exception e)
            {
                ShowCommandError(e);
            }
        }

        void ChangeVolume(int delta)
        {
            var dlna = client;
            if (dlna == null || !dlna.HasVolumeControl) return;
            try
            {
                int current = currentState.Volume >= 0 ? currentState.Volume : dlna.GetVolume();
                int volume = Math.Min(MaxVolume, Math.Max(0, current + delta));
                dlna.SetVolume(volume);
                Publish(With(s => { s.Volume = volume; s.HasVolumeControl = true; }));
            }
            catch (Exception e)
            {
                ShowCommandError(e);
            }
        }

        void ShowCommandError(Exception e)
        {
            var message = e is UpnpException u ? string.Format(CastStrings.ErrorTv, u.ErrorCode + " " + u.ErrorDescription) : CastStrings.ErrorUnreachable;
            Publish(With(s => s.Error = message));
        }

        void Fail(string message)
        {
            CastLog.E("Session failed: " + message);
            EndSession(keepState: true);
            // not active anymore: the controller shows the error and disables the buttons
            Publish(With(s => { s.Active = false; s.Status = CastStatus.Error; s.Error = message; }));
            Host?.SessionEnded();
        }

        void StopSession(bool stopTv)
        {
            if (stopTv && client != null) Try(client.Stop);
            EndSession();
            Host?.SessionEnded();
        }

        /// <summary>Releases everything; keepState keeps the state visible, e.g. to show an error.</summary>
        void EndSession(bool keepState = false)
        {
            if (client != null) SaveProgress();
            polling?.Dispose();
            polling = null;
            client = null;
            device = null;
            proxy?.Stop();
            proxy = null;
            queue.Clear();
            queueIndex = 0;
            if (!keepState) Publish(new CastState { Active = false });
        }

        // /////////////////////////////////////////////////////////////////////////
        // Queue and history
        // /////////////////////////////////////////////////////////////////////////

        List<CastQueueEntry> QueueEntries() => queue.Select(i => new CastQueueEntry(i.Title, i.Uploader, i.DurationSeconds)).ToList();

        /// <returns>the state showing the current video of the queue and what can be chosen for it</returns>
        CastState StateForCurrent(CastItem item)
        {
            var choice = currentChoice;
            var audioTrackId = choice?.Dash != null ? wantedAudioTrackId ?? item.AudioTrackIds.FirstOrDefault() : item.AudioTrackIds.FirstOrDefault();
            return With(s =>
            {
                s.Active = true;
                s.DeviceName = device?.FriendlyName ?? "";
                s.Url = item.Info.Url ?? "";
                s.Title = item.Title;
                s.Uploader = item.Uploader;
                s.ThumbnailUrl = item.Info.ThumbnailUrl;
                s.DurationSeconds = item.DurationSeconds;
                s.Queue = QueueEntries();
                s.QueueIndex = queueIndex;
                s.Qualities = item.Qualities;
                s.Quality = choice?.Height ?? 0;
                s.AutoQuality = wantedHeight == null;
                s.AudioTracks = item.AudioTrackIds.Select(item.AudioTrackName).ToList();
                s.AudioTrack = Math.Max(0, item.AudioTrackIds.IndexOf(audioTrackId));
                s.Subtitles = item.Subtitles.Select(CastItem.SubtitleName).ToList();
                s.Subtitle = currentSubtitles != null ? item.Subtitles.IndexOf(currentSubtitles) : -1;
            });
        }

        long SavedPosition(CastItem item)
        {
            try { return Host?.SavedPositionSeconds(item) ?? 0; }
            catch (Exception e)
            {
                CastLog.W("Could not load the saved position: " + e.Message);
                return 0;
            }
        }

        /// <summary>Saves the position reached on the TV, like the local player does.</summary>
        void SaveProgress()
        {
            if (queueIndex < 0 || queueIndex >= queue.Count) return;
            var item = queue[queueIndex];
            long position = lastPosition;
            if (position <= 0 || !hasPlayed) return;
            try { Host?.SaveProgress(item, position); }
            catch (Exception e) { CastLog.W("Could not save the position: " + e.Message); }
        }

        void MarkAsViewed(CastItem item)
        {
            try { Host?.MarkAsViewed(item); }
            catch (Exception e) { CastLog.W("Could not add the cast video to the history: " + e.Message); }
        }

        // /////////////////////////////////////////////////////////////////////////

        CastState With(Action<CastState> change)
        {
            var copy = currentState.Copy();
            change(copy);
            return copy;
        }

        void Publish(CastState state)
        {
            currentState = state;
            StateChanged?.Invoke(state);
        }

        static void Try(Action action)
        {
            try { action(); } catch (Exception) { }
        }

        static T TryGet<T>(Func<T> get, T fallback)
        {
            try { return get(); } catch (Exception) { return fallback; }
        }
    }
}
