/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.ts.TsWriter.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Perle.Cast.Ts
{
    /// <summary>
    /// Writes an MPEG transport stream with one H.264 video and one AAC (ADTS) audio stream.
    /// Times are in 90 kHz units. The PCR is carried by the video stream.
    /// </summary>
    public sealed class TsWriter
    {
        public const int PacketSize = 188;
        public const byte SyncByte = 0x47;
        public const int PatPid = 0x0000;
        public const int SdtPid = 0x0011;
        const string ServiceProvider = "NewPipe";
        const string ServiceName = "NewPipe";
        public const int PmtPid = 0x1000;
        public const int VideoPid = 0x0100;
        public const int AudioPid = 0x0101;
        public const int StreamTypeH264 = 0x1B;
        public const int StreamTypeAac = 0x0F;
        const int ProgramNumber = 1;
        const int VideoStreamId = 0xE0;
        const int AudioStreamId = 0xC0;

        /// <summary>Clock of PTS, DTS and PCR base.</summary>
        public const long Clock = 90000L;

        readonly Stream output;
        readonly Dictionary<int, int> continuity = new Dictionary<int, int>();
        readonly byte[] packet = new byte[PacketSize];

        public TsWriter(Stream output) { this.output = output; }

        /// <summary>Writes the service description table (DVB), which some players expect like ffmpeg sends it.</summary>
        public void WriteSdt() => WriteSection(SdtPid, Sdt());

        /// <summary>Writes the program tables, needed by players to find the streams.</summary>
        public void WriteTables()
        {
            WriteSection(PatPid, Pat());
            WriteSection(PmtPid, Pmt());
        }

        /// <param name="annexB">the access unit, with start codes</param>
        /// <param name="pcr">the program clock reference to send with it, or null</param>
        public void WriteVideo(byte[] annexB, long pts, long dts, bool isKeyframe, long? pcr) =>
            WritePes(VideoPid, VideoStreamId, annexB, pts, dts, isKeyframe, pcr);

        /// <param name="adts">one or more AAC frames, each with its ADTS header</param>
        public void WriteAudio(byte[] adts, long pts) => WritePes(AudioPid, AudioStreamId, adts, pts, pts, true, null);

        public void Flush() => output.Flush();

        void WritePes(int pid, int streamId, byte[] payload, long pts, long dts, bool randomAccess, long? pcr)
        {
            bool hasDts = dts != pts;
            int headerDataLength = hasDts ? 10 : 5;
            var header = new byte[9 + headerDataLength];
            header[0] = 0;
            header[1] = 0;
            header[2] = 1;
            header[3] = (byte)streamId;
            int pesLength = 3 + headerDataLength + payload.Length;
            // 0 (unbounded) is allowed only for video, which can be bigger than 64 KiB
            int lengthField = pesLength > 0xFFFF ? 0 : pesLength;
            header[4] = (byte)(lengthField >> 8);
            header[5] = (byte)lengthField;
            header[6] = 0x80; // marker bits '10'
            header[7] = (byte)(hasDts ? 0xC0 : 0x80);
            header[8] = (byte)headerDataLength;
            WriteTimestamp(header, 9, hasDts ? 0x3 : 0x2, pts);
            if (hasDts) WriteTimestamp(header, 14, 0x1, dts);

            var pes = new byte[header.Length + payload.Length];
            Buffer.BlockCopy(header, 0, pes, 0, header.Length);
            Buffer.BlockCopy(payload, 0, pes, header.Length, payload.Length);
            int offset = 0;
            bool first = true;
            while (offset < pes.Length)
            {
                Fill(packet, 0xFF);
                packet[0] = SyncByte;
                packet[1] = (byte)((first ? 0x40 : 0) | (pid >> 8 & 0x1F));
                packet[2] = (byte)pid;

                // adaptation field: PCR and random access flag on the first packet, then stuffing
                long? firstPcr = first ? pcr : null;
                int fieldFlags = (first && randomAccess ? 0x40 : 0) | (firstPcr != null ? 0x10 : 0);
                int minFieldLength = firstPcr != null ? 8 // flags + 6 bytes of PCR + length byte
                    : fieldFlags != 0 ? 2
                    : 0;
                int remaining = pes.Length - offset;
                int space = PacketSize - 4 - minFieldLength;
                int fieldLength = remaining < space ? minFieldLength + (space - remaining) : minFieldLength;
                int payloadSize = PacketSize - 4 - fieldLength;

                packet[3] = (byte)((fieldLength > 0 ? 0x30 : 0x10) | NextContinuity(pid));
                if (fieldLength > 0)
                {
                    packet[4] = (byte)(fieldLength - 1);
                    if (fieldLength > 1)
                    {
                        packet[5] = (byte)fieldFlags;
                        if (firstPcr != null) WritePcr(packet, 6, firstPcr.Value);
                        // the rest up to the payload is stuffing, already 0xFF
                    }
                }
                Buffer.BlockCopy(pes, offset, packet, 4 + fieldLength, payloadSize);
                output.Write(packet, 0, PacketSize);
                offset += payloadSize;
                first = false;
            }
        }

        void WriteSection(int pid, byte[] section)
        {
            Fill(packet, 0xFF);
            packet[0] = SyncByte;
            packet[1] = (byte)(0x40 | (pid >> 8 & 0x1F));
            packet[2] = (byte)pid;
            packet[3] = (byte)(0x10 | NextContinuity(pid));
            packet[4] = 0; // pointer field
            Buffer.BlockCopy(section, 0, packet, 5, section.Length);
            output.Write(packet, 0, PacketSize);
        }

        static void Fill(byte[] a, byte v) { for (int i = 0; i < a.Length; i++) a[i] = v; }

        int NextContinuity(int pid)
        {
            continuity.TryGetValue(pid, out var value);
            continuity[pid] = (value + 1) & 0x0F;
            return value;
        }

        static byte[] Pat() => WithCrc(new byte[]
        {
            0x00, // table id
            0xB0, 13, // section syntax, length
            0x00, 0x01, // transport stream id
            0xC1, // version 0, current
            0x00, 0x00, // section numbers
            0x00, ProgramNumber,
            (byte)(0xE0 | (PmtPid >> 8)), (byte)(PmtPid & 0xFF)
        });

        static byte[] Pmt() => WithCrc(new byte[]
        {
            0x02, // table id
            0xB0, 23, // section syntax, length
            0x00, ProgramNumber,
            0xC1,
            0x00, 0x00,
            (byte)(0xE0 | (VideoPid >> 8)), (byte)(VideoPid & 0xFF), // PCR PID
            0xF0, 0x00, // program info length
            StreamTypeH264, (byte)(0xE0 | (VideoPid >> 8)), (byte)(VideoPid & 0xFF), 0xF0, 0x00,
            StreamTypeAac, (byte)(0xE0 | (AudioPid >> 8)), (byte)(AudioPid & 0xFF), 0xF0, 0x00
        });

        static byte[] Sdt()
        {
            var provider = Encoding.ASCII.GetBytes(ServiceProvider);
            var name = Encoding.ASCII.GetBytes(ServiceName);
            // service descriptor: digital television service, provider and service names
            var descriptor = new byte[] { 0x48, (byte)(3 + provider.Length + name.Length), 0x01, (byte)provider.Length }
                .Concat(provider).Concat(new[] { (byte)name.Length }).Concat(name).ToArray();
            int sectionLength = 8 + 5 + descriptor.Length + 4;
            return WithCrc(new byte[]
            {
                0x42, // table id: SDT of this transport stream
                (byte)(0xF0 | (sectionLength >> 8)), (byte)sectionLength,
                0x00, 0x01, // transport stream id
                0xC1, // version 0, current
                0x00, 0x00, // section numbers
                0xFF, 0x01, // original network id, as ffmpeg
                0xFF, // reserved
                0x00, ProgramNumber, // service id
                0xFC, // no EIT
                (byte)(0x80 | (descriptor.Length >> 8)), (byte)descriptor.Length // running, not scrambled
            }.Concat(descriptor).ToArray());
        }

        static void WriteTimestamp(byte[] target, int offset, int prefix, long value)
        {
            long time = value & 0x1FFFFFFFFL;
            target[offset] = (byte)((prefix << 4) | ((int)(time >> 29) & 0x0E) | 0x01);
            target[offset + 1] = (byte)(time >> 22);
            target[offset + 2] = (byte)(((int)(time >> 14) & 0xFE) | 0x01);
            target[offset + 3] = (byte)(time >> 7);
            target[offset + 4] = (byte)(((int)(time << 1) & 0xFE) | 0x01);
        }

        static void WritePcr(byte[] target, int offset, long value)
        {
            long b = value & 0x1FFFFFFFFL;
            target[offset] = (byte)(b >> 25);
            target[offset + 1] = (byte)(b >> 17);
            target[offset + 2] = (byte)(b >> 9);
            target[offset + 3] = (byte)(b >> 1);
            target[offset + 4] = (byte)(((int)(b & 1L) << 7) | 0x7E); // reserved bits, extension 0
            target[offset + 5] = 0;
        }

        /// <summary>Appends the MPEG-2 CRC32 of section.</summary>
        public static byte[] WithCrc(byte[] section)
        {
            int crc = Crc32(section);
            return section.Concat(new[] { (byte)(crc >> 24), (byte)(crc >> 16), (byte)(crc >> 8), (byte)crc }).ToArray();
        }

        public static int Crc32(byte[] data)
        {
            int crc = -1;
            foreach (byte b in data)
            {
                crc ^= b << 24;
                for (int i = 0; i < 8; i++) crc = crc < 0 ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
            }
            return crc;
        }
    }
}
