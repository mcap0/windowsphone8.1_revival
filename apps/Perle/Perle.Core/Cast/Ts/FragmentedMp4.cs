/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.ts.FragmentedMp4.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Perle.Cast.Ts
{
    /// <summary>What is needed to put an H.264 or AAC track into MPEG-TS, read from the stsd box.</summary>
    public abstract class CodecConfig
    {
        /// <param name="nalLengthSize">the size of the length prefix of every NAL unit in the samples</param>
        /// <param name="sps">the sequence parameter sets, without start codes</param>
        /// <param name="pps">the picture parameter sets, without start codes</param>
        public sealed class Avc : CodecConfig
        {
            public readonly int NalLengthSize;
            public readonly List<byte[]> Sps, Pps;
            public Avc(int nalLengthSize, List<byte[]> sps, List<byte[]> pps) { NalLengthSize = nalLengthSize; Sps = sps; Pps = pps; }
        }

        /// <param name="objectType">the MPEG-4 audio object type, e.g. 2 for AAC-LC</param>
        /// <param name="frequencyIndex">the sampling frequency index (e.g. 4 for 44100 Hz)</param>
        /// <param name="channels">the channel configuration (e.g. 2 for stereo)</param>
        public sealed class Aac : CodecConfig
        {
            public readonly int ObjectType, FrequencyIndex, Channels;
            public Aac(int objectType, int frequencyIndex, int channels) { ObjectType = objectType; FrequencyIndex = frequencyIndex; Channels = channels; }
        }
    }

    /// <summary>The track of a fragmented MP4 file (as served by YouTube: one track per file).</summary>
    public sealed class Mp4Track
    {
        public readonly int TrackId;
        /// <summary>ticks per second of the sample times</summary>
        public readonly long Timescale;
        public readonly CodecConfig Codec;
        public readonly int DefaultSampleDuration, DefaultSampleSize;

        public Mp4Track(int trackId, long timescale, CodecConfig codec, int defaultSampleDuration, int defaultSampleSize)
        {
            TrackId = trackId; Timescale = timescale; Codec = codec;
            DefaultSampleDuration = defaultSampleDuration; DefaultSampleSize = defaultSampleSize;
        }
    }

    /// <summary>One reference of the segment index (sidx): a moof + mdat fragment.</summary>
    /// <param name="Offset">the position of the fragment in the file</param>
    /// <param name="StartSeconds">the presentation time at which the fragment starts</param>
    public sealed class Mp4Segment
    {
        public readonly long Offset, Size;
        public readonly double StartSeconds, DurationSeconds;
        public Mp4Segment(long offset, long size, double startSeconds, double durationSeconds)
        { Offset = offset; Size = size; StartSeconds = startSeconds; DurationSeconds = durationSeconds; }
    }

    /// <param name="DecodeTime">in track timescale units</param>
    /// <param name="CompositionOffset">presentation time minus decode time, in track timescale units</param>
    public sealed class Mp4Sample
    {
        public readonly long DecodeTime;
        public readonly int CompositionOffset;
        public readonly byte[] Data;
        public Mp4Sample(long decodeTime, int compositionOffset, byte[] data) { DecodeTime = decodeTime; CompositionOffset = compositionOffset; Data = data; }
    }

    /// <summary>
    /// Minimal parser for the fragmented MP4 files used by DASH, enough to read H.264 and AAC samples
    /// one fragment at a time from a forward-only stream.
    /// </summary>
    public static class FragmentedMp4
    {
        const int MOOV = 0x6D6F6F76;
        const int TRAK = 0x7472616B;
        const int TKHD = 0x746B6864;
        const int MDIA = 0x6D646961;
        const int MDHD = 0x6D646864;
        const int MINF = 0x6D696E66;
        const int STBL = 0x7374626C;
        const int STSD = 0x73747364;
        const int MVEX = 0x6D766578;
        const int TREX = 0x74726578;
        const int AVC1 = 0x61766331;
        const int AVC3 = 0x61766333;
        const int AVCC = 0x61766343;
        const int MP4A = 0x6D703461;
        const int ESDS = 0x65736473;
        const int SIDX = 0x73696478;
        internal const int MOOF = 0x6D6F6F66;
        internal const int MDAT = 0x6D646174;
        const int TRAF = 0x74726166;
        const int TFHD = 0x74666864;
        const int TFDT = 0x74666474;
        const int TRUN = 0x7472756E;

        /// <summary>Size of the fields of a VisualSampleEntry before its child boxes.</summary>
        const int VisualSampleEntrySize = 78;

        /// <summary>Size of the fields of an AudioSampleEntry before its child boxes.</summary>
        const int AudioSampleEntrySize = 28;

        /// <summary>Parses the initialization data (ftyp + moov).</summary>
        /// <exception cref="IOException">if there is no H.264 or AAC track</exception>
        public static Mp4Track ParseInit(byte[] init)
        {
            var moov = Boxes(ByteBuf.Wrap(init)).FirstOrDefault(b => b.Type == MOOV)
                ?? throw new IOException("No moov box in the initialization data");
            var trak = Boxes(moov.Body).FirstOrDefault(b => b.Type == TRAK)
                ?? throw new IOException("No track in moov");

            var tkhd = Child(trak.Body, TKHD);
            int tkhdVersion = tkhd.Get(0);
            int trackId = tkhd.GetInt(tkhdVersion == 1 ? 4 + 16 : 4 + 8);

            var mdia = Child(trak.Body, MDIA);
            var mdhd = Child(mdia, MDHD);
            long timescale = (uint)mdhd.GetInt(mdhd.Get(0) == 1 ? 4 + 16 : 4 + 8);

            var stsd = Child(Child(Child(mdia, MINF), STBL), STSD);
            // version, flags and entry count, then the first sample entry
            stsd.Position(8);
            var entry = Boxes(stsd.Slice()).FirstOrDefault() ?? throw new IOException("Empty stsd");
            CodecConfig codec;
            switch (entry.Type)
            {
                case AVC1:
                case AVC3: codec = ParseAvcC(Child(entry.Body, AVCC, VisualSampleEntrySize)); break;
                case MP4A: codec = ParseEsds(Child(entry.Body, ESDS, AudioSampleEntrySize)); break;
                default: throw new IOException("Unsupported codec " + TypeName(entry.Type));
            }

            int defaultDuration = 0, defaultSize = 0;
            var mvex = Boxes(moov.Body).FirstOrDefault(b => b.Type == MVEX);
            if (mvex != null)
            {
                foreach (var trex in Boxes(mvex.Body).Where(b => b.Type == TREX))
                {
                    var body = trex.Body;
                    if (body.GetInt(4) == trackId)
                    {
                        defaultDuration = body.GetInt(12);
                        defaultSize = body.GetInt(16);
                    }
                }
            }
            return new Mp4Track(trackId, timescale, codec, defaultDuration, defaultSize);
        }

        /// <summary>Parses the segment index.</summary>
        /// <param name="sidx">the bytes of the sidx box</param>
        /// <param name="sidxOffset">the position of the sidx box in the file</param>
        public static List<Mp4Segment> ParseSidx(byte[] sidx, long sidxOffset)
        {
            var box = Boxes(ByteBuf.Wrap(sidx)).FirstOrDefault(b => b.Type == SIDX)
                ?? throw new IOException("No sidx box in the index data");
            var body = box.Body;
            int version = body.Get();
            body.Position(body.Position() + 3 + 4); // flags, reference ID
            long timescale = (uint)body.GetInt();
            if (timescale <= 0) throw new IOException("Invalid sidx timescale");
            long earliestTime, firstOffset;
            if (version == 0)
            {
                earliestTime = (uint)body.GetInt();
                firstOffset = (uint)body.GetInt();
            }
            else
            {
                earliestTime = body.GetLong();
                firstOffset = body.GetLong();
            }
            body.Position(body.Position() + 2); // reserved
            int count = (ushort)body.GetShort();

            long offset = sidxOffset + box.Size + firstOffset;
            long time = earliestTime;
            var segments = new List<Mp4Segment>(count);
            for (int i = 0; i < count; i++)
            {
                long size = body.GetInt() & 0x7FFFFFFF;
                long duration = (uint)body.GetInt();
                body.GetInt(); // SAP
                segments.Add(new Mp4Segment(offset, size, (double)time / timescale, (double)duration / timescale));
                offset += size;
                time += duration;
            }
            return segments;
        }

        /// <summary>Reads the samples of the fragments that follow, one moof + mdat pair at a time.</summary>
        public sealed class FragmentReader
        {
            readonly Stream input;
            readonly Mp4Track track;

            /// <summary>Decode time of the next sample, for fragments without tfdt.</summary>
            long nextDecodeTime;

            public FragmentReader(Stream input, Mp4Track track) { this.input = input; this.track = track; }

            /// <returns>the samples of the next fragment, or null at the end of the stream</returns>
            public List<Mp4Sample> NextFragment()
            {
                while (true)
                {
                    var moofHeader = ReadHeader();
                    if (moofHeader == null) return null;
                    if (moofHeader.Type != MOOF)
                    {
                        SkipFully(moofHeader.BodySize);
                        continue;
                    }
                    var moof = ByteBuf.Wrap(ReadFully(moofHeader.BodySize));
                    var runs = ParseMoof(moof);

                    long skipped = 0;
                    var mdatHeader = ReadHeader() ?? throw new EndOfStreamException("moof without mdat");
                    while (mdatHeader.Type != MDAT)
                    {
                        SkipFully(mdatHeader.BodySize);
                        skipped += mdatHeader.HeaderSize + mdatHeader.BodySize;
                        mdatHeader = ReadHeader() ?? throw new EndOfStreamException("moof without mdat");
                    }
                    var mdat = ReadFully(mdatHeader.BodySize);
                    // the data offsets of trun are relative to the start of moof
                    long mdatDataStart = moofHeader.HeaderSize + moofHeader.BodySize + skipped + mdatHeader.HeaderSize;
                    return BuildSamples(runs, mdat, mdatDataStart);
                }
            }

            sealed class Run
            {
                public long DecodeTime;
                public long? DataOffset;
                public int[] Durations, Sizes, CompositionOffsets;
            }

            List<Run> ParseMoof(ByteBuf moof)
            {
                var runs = new List<Run>();
                foreach (var traf in Boxes(moof).Where(b => b.Type == TRAF))
                {
                    var children = Boxes(traf.Body);
                    var tfhd = children.FirstOrDefault(b => b.Type == TFHD)?.Body;
                    if (tfhd == null) continue;
                    int tfhdFlags = tfhd.GetInt() & 0xFFFFFF;
                    if (tfhd.GetInt() != track.TrackId) continue;
                    if ((tfhdFlags & 0x01) != 0) tfhd.GetLong(); // base data offset: only moof-relative files are supported
                    if ((tfhdFlags & 0x02) != 0) tfhd.GetInt(); // sample description index
                    int defaultDuration = (tfhdFlags & 0x08) != 0 ? tfhd.GetInt() : track.DefaultSampleDuration;
                    int defaultSize = (tfhdFlags & 0x10) != 0 ? tfhd.GetInt() : track.DefaultSampleSize;

                    long decodeTime = nextDecodeTime;
                    var tfdt = children.FirstOrDefault(b => b.Type == TFDT)?.Body;
                    if (tfdt != null)
                    {
                        bool v1 = tfdt.Get() == 1;
                        tfdt.Position(4);
                        decodeTime = v1 ? tfdt.GetLong() : (uint)tfdt.GetInt();
                    }

                    foreach (var trun in children.Where(b => b.Type == TRUN))
                    {
                        var body = trun.Body;
                        int flags = body.GetInt() & 0xFFFFFF;
                        int count = body.GetInt();
                        long? dataOffset = (flags & 0x01) != 0 ? body.GetInt() : (long?)null;
                        if ((flags & 0x04) != 0) body.GetInt(); // first sample flags
                        var durations = new int[count];
                        var sizes = new int[count];
                        var offsets = new int[count];
                        for (int i = 0; i < count; i++)
                        {
                            durations[i] = (flags & 0x100) != 0 ? body.GetInt() : defaultDuration;
                            sizes[i] = (flags & 0x200) != 0 ? body.GetInt() : defaultSize;
                            if ((flags & 0x400) != 0) body.GetInt(); // sample flags
                            offsets[i] = (flags & 0x800) != 0 ? body.GetInt() : 0;
                        }
                        runs.Add(new Run { DecodeTime = decodeTime, DataOffset = dataOffset, Durations = durations, Sizes = sizes, CompositionOffsets = offsets });
                        decodeTime += durations.Sum(d => (long)d);
                    }
                    nextDecodeTime = decodeTime;
                }
                return runs;
            }

            static List<Mp4Sample> BuildSamples(List<Run> runs, byte[] mdat, long mdatDataStart)
            {
                var samples = new List<Mp4Sample>();
                long position = 0; // inside mdat, used when a trun has no data offset
                foreach (var run in runs)
                {
                    if (run.DataOffset != null) position = run.DataOffset.Value - mdatDataStart;
                    long time = run.DecodeTime;
                    for (int i = 0; i < run.Sizes.Length; i++)
                    {
                        int size = run.Sizes[i];
                        if (position < 0 || position + size > mdat.Length) throw new IOException("Sample outside of mdat");
                        var data = new byte[size];
                        Buffer.BlockCopy(mdat, (int)position, data, 0, size);
                        samples.Add(new Mp4Sample(time, run.CompositionOffsets[i], data));
                        position += size;
                        time += run.Durations[i];
                    }
                }
                return samples;
            }

            sealed class Header
            {
                public int Type;
                public long HeaderSize, BodySize;
            }

            Header ReadHeader()
            {
                int first = input.ReadByte();
                if (first < 0) return null;
                long size32 = ((long)first << 24) | ((long)ReadUnsignedByte() << 16) | ((long)ReadUnsignedByte() << 8) | (long)ReadUnsignedByte();
                int type = ReadInt();
                if (size32 == 1) return new Header { Type = type, HeaderSize = 16, BodySize = ReadLong() - 16 };
                if (size32 == 0) throw new IOException("Boxes extending to the end of the file are not supported");
                return new Header { Type = type, HeaderSize = 8, BodySize = size32 - 8 };
            }

            int ReadUnsignedByte()
            {
                int b = input.ReadByte();
                if (b < 0) throw new EndOfStreamException();
                return b;
            }

            int ReadInt() => (ReadUnsignedByte() << 24) | (ReadUnsignedByte() << 16) | (ReadUnsignedByte() << 8) | ReadUnsignedByte();

            long ReadLong() => ((long)ReadInt() << 32) | (uint)ReadInt();

            byte[] ReadFully(long size)
            {
                if (size < 0 || size > MaxBoxSize) throw new IOException("Invalid box size " + size);
                var buffer = new byte[size];
                int read = 0;
                while (read < buffer.Length)
                {
                    int n = input.Read(buffer, read, buffer.Length - read);
                    if (n <= 0) throw new EndOfStreamException();
                    read += n;
                }
                return buffer;
            }

            void SkipFully(long size)
            {
                var scratch = new byte[(int)Math.Min(size, 64 * 1024)];
                long remaining = size;
                while (remaining > 0)
                {
                    int n = input.Read(scratch, 0, (int)Math.Min(remaining, scratch.Length));
                    if (n <= 0) throw new EndOfStreamException();
                    remaining -= n;
                }
            }
        }

        /// <summary>Fragments bigger than this are not expected (YouTube's are a few MB).</summary>
        const long MaxBoxSize = 64L * 1024 * 1024;

        sealed class Box
        {
            public int Type;
            public long Size;
            public ByteBuf Body;
        }

        /// <returns>the boxes in buffer, from its position to its limit</returns>
        static List<Box> Boxes(ByteBuf buffer)
        {
            var data = buffer.Slice();
            var result = new List<Box>();
            while (data.Remaining() >= 8)
            {
                int start = data.Position();
                long size = (uint)data.GetInt();
                int type = data.GetInt();
                int headerSize = 8;
                if (size == 1)
                {
                    size = data.GetLong();
                    headerSize = 16;
                }
                else if (size == 0)
                {
                    size = data.Limit() - start;
                }
                if (size < headerSize || start + size > data.Limit()) break;
                var body = data.Duplicate();
                body.Position(start + headerSize);
                body.Limit((int)(start + size));
                result.Add(new Box { Type = type, Size = size, Body = body.Slice() });
                data.Position((int)(start + size));
            }
            return result;
        }

        static ByteBuf Child(ByteBuf parent, int type, int skip = 0)
        {
            var data = parent.Duplicate();
            data.Position(skip);
            return Boxes(data).FirstOrDefault(b => b.Type == type)?.Body
                ?? throw new IOException("No " + TypeName(type) + " box");
        }

        static CodecConfig.Avc ParseAvcC(ByteBuf avcC)
        {
            avcC.Position(4); // version, profile, compatibility, level
            int nalLengthSize = (avcC.Get() & 0x03) + 1;
            var sps = ReadParameterSets(avcC, avcC.Get() & 0x1F);
            var pps = ReadParameterSets(avcC, avcC.Get() & 0xFF);
            return new CodecConfig.Avc(nalLengthSize, sps, pps);
        }

        static List<byte[]> ReadParameterSets(ByteBuf buffer, int count)
        {
            var list = new List<byte[]>(count);
            for (int i = 0; i < count; i++)
            {
                int length = (ushort)buffer.GetShort();
                var b = new byte[length];
                buffer.Get(b);
                list.Add(b);
            }
            return list;
        }

        /// <summary>Finds the AudioSpecificConfig in the descriptors of the esds box.</summary>
        static CodecConfig.Aac ParseEsds(ByteBuf esds)
        {
            esds.Position(4); // version and flags
            while (esds.HasRemaining())
            {
                int tag = esds.Get() & 0xFF;
                int length = 0;
                for (int i = 0; i < 4; i++)
                {
                    int b = esds.Get() & 0xFF;
                    length = (length << 7) | (b & 0x7F);
                    if ((b & 0x80) == 0) break;
                }
                switch (tag)
                {
                    // ES_Descriptor: ES_ID, flags and the optional fields they announce
                    case 0x03:
                        {
                            esds.GetShort();
                            int flags = esds.Get() & 0xFF;
                            if ((flags & 0x80) != 0) esds.GetShort();
                            if ((flags & 0x40) != 0) esds.Position(esds.Position() + (esds.Get() & 0xFF));
                            if ((flags & 0x20) != 0) esds.GetShort();
                            break;
                        }

                    // DecoderConfigDescriptor: fixed fields, then the DecoderSpecificInfo
                    case 0x04: esds.Position(esds.Position() + 13); break;

                    // DecoderSpecificInfo: the AudioSpecificConfig
                    case 0x05:
                        {
                            int b0 = esds.Get() & 0xFF;
                            int b1 = esds.Get() & 0xFF;
                            int objectType = b0 >> 3;
                            int frequencyIndex = ((b0 & 0x07) << 1) | (b1 >> 7);
                            int channels = (b1 >> 3) & 0x0F;
                            if (objectType == 31 || frequencyIndex == 15) throw new IOException("Unsupported AudioSpecificConfig");
                            // HE-AAC is signaled implicitly in YouTube files, AAC-LC is what ADTS needs
                            if (objectType > 4) objectType = 2;
                            if (channels == 0) channels = 2;
                            return new CodecConfig.Aac(objectType, frequencyIndex, channels);
                        }

                    default: esds.Position(esds.Position() + length); break;
                }
            }
            throw new IOException("No AudioSpecificConfig in esds");
        }

        // ISO-8859-1 maps bytes to the same code points (and is not available on .NET Native)
        static string TypeName(int type) => new string(new[]
            { (char)((type >> 24) & 0xFF), (char)((type >> 16) & 0xFF), (char)((type >> 8) & 0xFF), (char)(type & 0xFF) });
    }
}
