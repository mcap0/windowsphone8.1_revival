/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.ts.LiveTsMuxer.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using Perle.Cast.Proxy;

namespace Perle.Cast.Ts
{
    /// <summary>
    /// Turns a video-only H.264 MP4 and an AAC M4A stream (DASH, one file each) into a single MPEG-TS
    /// stream while they are being downloaded, so that TVs which need one file with audio and video
    /// can play them without waiting for a download.
    ///
    /// The streams are read from the fragment containing the requested start time, which is how
    /// seeking works: the TV gets a new stream starting there.
    /// </summary>
    public sealed class LiveTsMuxer
    {
        sealed class Input
        {
            public readonly Mp4Track Track;
            public readonly List<Mp4Segment> Segments;
            public readonly string Url;
            public Input(Mp4Track track, List<Mp4Segment> segments, string url) { Track = track; Segments = segments; Url = url; }
        }

        readonly IUpstream upstream;
        readonly CastDash tracks;
        readonly object gate = new object();
        Input video;
        Input audio;

        public LiveTsMuxer(IUpstream upstream, CastDash tracks) { this.upstream = upstream; this.tracks = tracks; }

        static string F1(double d) => d.ToString("0.0", CultureInfo.InvariantCulture);
        static string F2(double d) => d.ToString("0.00", CultureInfo.InvariantCulture);
        static string F3(double d) => d.ToString("0.000", CultureInfo.InvariantCulture);
        static string List(IEnumerable<int> l) => "[" + string.Join(", ", l) + "]";

        /// <summary>Downloads and parses the initialization data and the segment index of both streams.</summary>
        void Prepare(out Input v, out Input a)
        {
            lock (gate)
            {
                v = video ?? (video = Load(tracks.Video));
                a = audio ?? (audio = Load(tracks.Audio));
            }
            if (!(v.Track.Codec is CodecConfig.Avc)) throw new IOException("The video stream is not H.264");
            if (!(a.Track.Codec is CodecConfig.Aac)) throw new IOException("The audio stream is not AAC");
        }

        Input Load(DashTrack dashTrack)
        {
            var init = RangedInputStream.ReadRange(upstream, dashTrack.Url, dashTrack.InitStart, dashTrack.InitEnd);
            var index = RangedInputStream.ReadRange(upstream, dashTrack.Url, dashTrack.IndexStart, dashTrack.IndexEnd);
            var track = FragmentedMp4.ParseInit(init);
            var segments = FragmentedMp4.ParseSidx(index, dashTrack.IndexStart);
            if (segments.Count == 0) throw new IOException("No segments in " + dashTrack.MimeType + " stream");
            return new Input(track, segments, dashTrack.Url);
        }

        /// <summary>
        /// Writes the stream from startSeconds until the end, or until writing fails because the TV
        /// closed the connection.
        /// </summary>
        public void Write(Stream output, double startSeconds)
        {
            Prepare(out var videoInput, out var audioInput);
            var videoSegment = videoInput.Segments.LastOrDefault(s => s.StartSeconds <= startSeconds) ?? videoInput.Segments.First();
            var audioSegment = audioInput.Segments.LastOrDefault(s => s.StartSeconds <= videoSegment.StartSeconds) ?? audioInput.Segments.First();
            double @base = videoSegment.StartSeconds;
            CastLog.D("TS mux from " + F1(@base) + " s (asked " + F1(startSeconds) + " s): "
                + "video at byte " + videoSegment.Offset + ", audio at byte " + audioSegment.Offset + "; "
                + "video segment " + (videoInput.Segments.IndexOf(videoSegment) + 1) + "/" + videoInput.Segments.Count
                + " of " + F1(videoSegment.DurationSeconds) + " s, first segments: "
                + string.Join(", ", videoInput.Segments.Take(LoggedSegments).Select(s => F1(s.DurationSeconds))) + " s");

            using (var videoStream = new RangedInputStream(upstream, videoInput.Url, videoSegment.Offset))
            using (var audioStream = new RangedInputStream(upstream, audioInput.Url, audioSegment.Offset))
            {
                var videoCursor = new Cursor(new FragmentedMp4.FragmentReader(new BufferedStream(videoStream, 64 * 1024), videoInput.Track), videoInput.Track);
                var audioCursor = new Cursor(new FragmentedMp4.FragmentReader(new BufferedStream(audioStream, 64 * 1024), audioInput.Track), audioInput.Track);
                Mux(videoCursor, audioCursor, @base, output);
            }
        }

        /// <summary>The samples of one stream, read one fragment at a time.</summary>
        sealed class Cursor
        {
            readonly FragmentedMp4.FragmentReader reader;
            public readonly Mp4Track Track;
            List<Mp4Sample> fragment = new List<Mp4Sample>();
            int index;

            public Cursor(FragmentedMp4.FragmentReader reader, Mp4Track track) { this.reader = reader; Track = track; }

            public Mp4Sample Peek()
            {
                while (index >= fragment.Count)
                {
                    fragment = reader.NextFragment();
                    if (fragment == null) { fragment = new List<Mp4Sample>(); index = 0; return null; }
                    index = 0;
                }
                return fragment[index];
            }

            public Mp4Sample Next()
            {
                var s = Peek();
                if (s != null) index++;
                return s;
            }

            public double Seconds(long time) => (double)time / Track.Timescale;
        }

        void Mux(Cursor video, Cursor audio, double baseSeconds, Stream rawOutput)
        {
            var output = new CountingStream(rawOutput);
            var writer = new TsWriter(output);
            var avc = (CodecConfig.Avc)video.Track.Codec;
            var aac = (CodecConfig.Aac)audio.Track.Codec;
            long Clock(double seconds) => (long)Math.Round((seconds - baseSeconds) * TsWriter.Clock, MidpointRounding.AwayFromZero) + StartDelay;

            // like ffmpeg: service description and program tables first, then again every 500 and
            // 100 ms, so that a player can start anywhere and finds them quickly
            writer.WriteSdt();
            writer.WriteTables();
            // MP4 files hide the B-frame delay of the first frame with an edit list: without it,
            // the video would be late compared to the audio
            var firstVideo = video.Peek();
            double videoShift = firstVideo != null ? video.Seconds(firstVideo.CompositionOffset) : 0.0;
            var firstAudio = audio.Peek();
            long lastSdt = firstVideo != null ? Clock(video.Seconds(firstVideo.DecodeTime) - videoShift) : 0L;
            long lastTables = lastSdt;
            CastLog.D("Mux start: base " + F3(baseSeconds) + " s, first video sample at "
                + (firstVideo != null ? F3(video.Seconds(firstVideo.DecodeTime)) + " s (+" + F3(videoShift) + " s)" : "none")
                + ", first audio sample at " + (firstAudio != null ? F3(audio.Seconds(firstAudio.DecodeTime)) + " s" : "none")
                + ", " + avc.Sps.Count + " SPS, " + avc.Pps.Count + " PPS, AAC type " + aac.ObjectType + " index " + aac.FrequencyIndex + " channels " + aac.Channels);
            var startedAt = DateTime.UtcNow;
            double nextReport = ProgressSeconds;
            long videoFrames = 0, audioFrames = 0, skippedAudio = 0, reorderedFrames = 0;
            while (true)
            {
                var videoSample = video.Peek();
                var audioSample = audio.Peek();
                if (videoSample == null && audioSample == null) break;

                double videoTime = videoSample != null ? video.Seconds(videoSample.DecodeTime) - videoShift : double.MaxValue;
                double audioTime = audioSample != null ? audio.Seconds(audioSample.DecodeTime) : double.MaxValue;

                if (videoTime <= audioTime)
                {
                    var sample = video.Next();
                    long dts = Clock(videoTime);
                    long pts = Clock(video.Seconds(sample.DecodeTime + sample.CompositionOffset) - videoShift);
                    var annexB = ToAnnexB(sample.Data, avc, out bool isKeyframe);
                    long pcr = Math.Max(dts - PcrDelay, 0);
                    var inputTypes = NalTypes(sample.Data, avc);
                    if (IsReordered(inputTypes))
                    {
                        reorderedFrames++;
                        if (reorderedFrames <= LoggedReorders)
                            CastLog.D("Mux frame " + videoFrames + " at " + F2(videoTime - baseSeconds) + " s: NAL types " + List(inputTypes) + ", sent " + List(SentNalTypes(annexB)));
                    }
                    if (videoFrames == 0)
                    {
                        CastLog.D("Mux first video frame: keyframe " + isKeyframe.ToString().ToLowerInvariant() + ", NAL types " + List(inputTypes) + ", sent " + List(SentNalTypes(annexB))
                            + ", PTS " + pts + ", DTS " + dts + ", PCR " + pcr + ", " + skippedAudio + " audio frames skipped");
                    }
                    if (dts - lastSdt >= SdtInterval)
                    {
                        writer.WriteSdt();
                        lastSdt = dts;
                    }
                    if (isKeyframe || dts - lastTables >= TablesInterval)
                    {
                        writer.WriteTables();
                        lastTables = dts;
                    }
                    writer.WriteVideo(annexB, pts, dts, isKeyframe, pcr);
                    writer.Flush();
                    videoFrames++;
                    double sent = videoTime - baseSeconds;
                    if (sent >= nextReport)
                    {
                        nextReport += ProgressSeconds;
                        CastLog.D("Mux progress: " + (long)sent + " s of video, " + videoFrames + " video and " + audioFrames + " audio frames, "
                            + (output.Count / 1024) + " KB sent in " + (long)(DateTime.UtcNow - startedAt).TotalSeconds + " s"
                            + (reorderedFrames > 0 ? ", " + reorderedFrames + " frames reordered" : ""));
                    }
                }
                else
                {
                    var sample = audio.Next();
                    // audio before the first video frame would have a negative time
                    if (audioTime >= baseSeconds)
                    {
                        if (audioFrames == 0) CastLog.D("Mux first audio frame: PTS " + Clock(audioTime) + ", after " + videoFrames + " video frames");
                        writer.WriteAudio(Adts(sample.Data, aac), Clock(audioTime));
                        audioFrames++;
                    }
                    else
                    {
                        skippedAudio++;
                    }
                }
            }
            writer.Flush();
            CastLog.D("TS mux finished: " + videoFrames + " video frames, " + audioFrames + " audio frames");
        }

        /// <summary>Counts the bytes sent to the TV, for the log.</summary>
        sealed class CountingStream : Stream
        {
            readonly Stream output;
            public long Count { get; private set; }
            public CountingStream(Stream output) { this.output = output; }
            public override void Write(byte[] buffer, int offset, int count) { output.Write(buffer, offset, count); Count += count; }
            public override void Flush() => output.Flush();
            public override bool CanRead => false;
            public override bool CanSeek => false;
            public override bool CanWrite => true;
            public override long Length => Count;
            public override long Position { get => Count; set => throw new NotSupportedException(); }
            public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
            public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
            public override void SetLength(long value) => throw new NotSupportedException();
        }

        /// <summary>
        /// Like ffmpeg: the first frame is presented 1.4 s after the start of the clock and decoded
        /// PcrDelay after it arrives, so the clock reference starts above 0 and always grows,
        /// even when the first decode time is earlier than the first presentation time (B-frames).
        /// </summary>
        public const long StartDelay = 126000L;

        /// <summary>Time between the arrival of a frame (PCR) and its decoding (DTS): 0.7 s like ffmpeg.</summary>
        public const long PcrDelay = 63000L;

        /// <summary>How often the program tables and the service description are repeated, in 90 kHz units.</summary>
        const long TablesInterval = 9000L;
        const long SdtInterval = 45000L;

        const int LoggedSegments = 6;
        const int LoggedReorders = 3;

        /// <summary>How often the progress of the stream is logged, in seconds of video.</summary>
        const double ProgressSeconds = 10.0;

        const int NalSlice = 1;
        const int NalIdr = 5;
        const int NalSps = 7;
        const int NalPps = 8;
        const int NalAud = 9;

        /// <summary>
        /// Access unit delimiters (one is written already), end of sequence and stream (which
        /// would end the video for the decoder) and filler data.
        /// </summary>
        static readonly HashSet<int> DroppedNals = new HashSet<int> { NalAud, 10, 11, 12 };
        static readonly byte[] StartCode = { 0, 0, 0, 1 };
        static readonly byte[] AccessUnitDelimiter = { 0, 0, 0, 1, 0x09, 0xF0 };

        static bool IsPicture(int type) => type >= NalSlice && type <= NalIdr;

        /// <summary>
        /// Converts an MP4 sample (NAL units prefixed by their length) to Annex B (NAL units
        /// prefixed by start codes), adding the parameter sets before keyframes.
        /// </summary>
        /// <returns>the converted access unit; isKeyframe tells whether it is a keyframe</returns>
        public static byte[] ToAnnexB(byte[] sample, CodecConfig.Avc avc, out bool isKeyframe)
        {
            var nals = new List<byte[]>();
            int position = 0;
            while (position + avc.NalLengthSize <= sample.Length)
            {
                int length = 0;
                for (int i = 0; i < avc.NalLengthSize; i++) length = (length << 8) | sample[position + i];
                position += avc.NalLengthSize;
                if (length <= 0 || position + length > sample.Length) break;
                var nal = new byte[length];
                Buffer.BlockCopy(sample, position, nal, 0, length);
                nals.Add(nal);
                position += length;
            }

            var types = nals.Select(n => n[0] & 0x1F).ToList();
            isKeyframe = types.Contains(NalIdr);
            var output = new MemoryStream(sample.Length + 64);
            output.Write(AccessUnitDelimiter, 0, AccessUnitDelimiter.Length);
            if (isKeyframe && !types.Contains(NalSps))
            {
                foreach (var sps in avc.Sps) { output.Write(StartCode, 0, 4); output.Write(sps, 0, sps.Length); }
                if (!types.Contains(NalPps))
                    foreach (var pps in avc.Pps) { output.Write(StartCode, 0, 4); output.Write(pps, 0, pps.Length); }
            }
            // H.264 wants the SEI and other non-picture units before the slices of the picture:
            // YouTube files sometimes have an SEI after them (e.g. in the first frame), which
            // made TVs stall at the start of the video
            var indices = Enumerable.Range(0, nals.Count);
            var ordered = indices.Where(i => !IsPicture(types[i])).Concat(indices.Where(i => IsPicture(types[i])));
            foreach (int i in ordered)
            {
                if (!DroppedNals.Contains(types[i]))
                {
                    output.Write(StartCode, 0, 4);
                    output.Write(nals[i], 0, nals[i].Length);
                }
            }
            return output.ToArray();
        }

        /// <returns>true if a non-picture unit comes after a slice, which ToAnnexB fixes</returns>
        public static bool IsReordered(List<int> types)
        {
            int firstSlice = types.FindIndex(IsPicture);
            return firstSlice >= 0 && types.Skip(firstSlice + 1).Any(t => !IsPicture(t) && !DroppedNals.Contains(t));
        }

        /// <returns>the types of the NAL units of an Annex B access unit (4-byte start codes), for the log</returns>
        public static List<int> SentNalTypes(byte[] annexB)
        {
            var types = new List<int>();
            int i = 0;
            while (i + 4 < annexB.Length)
            {
                if (annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 0 && annexB[i + 3] == 1)
                {
                    types.Add(annexB[i + 4] & 0x1F);
                    i += 4;
                }
                else i++;
            }
            return types;
        }

        /// <returns>the types of the NAL units of an MP4 sample, for the log</returns>
        public static List<int> NalTypes(byte[] sample, CodecConfig.Avc avc)
        {
            var types = new List<int>();
            int position = 0;
            while (position + avc.NalLengthSize < sample.Length)
            {
                int length = 0;
                for (int i = 0; i < avc.NalLengthSize; i++) length = (length << 8) | sample[position + i];
                position += avc.NalLengthSize;
                if (length <= 0 || position + length > sample.Length) break;
                types.Add(sample[position] & 0x1F);
                position += length;
            }
            return types;
        }

        /// <summary>Prefixes a raw AAC frame with its ADTS header.</summary>
        public static byte[] Adts(byte[] frame, CodecConfig.Aac aac)
        {
            int length = frame.Length + 7;
            var result = new byte[length];
            result[0] = 0xFF;
            result[1] = 0xF1; // MPEG-4, layer 0, no CRC
            result[2] = (byte)((((aac.ObjectType - 1) & 0x03) << 6) | (aac.FrequencyIndex << 2) | (aac.Channels >> 2 & 0x01));
            result[3] = (byte)(((aac.Channels & 0x03) << 6) | (length >> 11 & 0x03));
            result[4] = (byte)(length >> 3);
            result[5] = (byte)(((length & 0x07) << 5) | 0x1F); // buffer fullness 0x7FF
            result[6] = 0xFC;
            Buffer.BlockCopy(frame, 0, result, 7, frame.Length);
            return result;
        }
    }
}
