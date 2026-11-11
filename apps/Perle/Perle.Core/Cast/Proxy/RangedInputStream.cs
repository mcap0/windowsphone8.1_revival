/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.proxy.RangedInputStream.
 */

using System;
using System.IO;
using System.Threading;

namespace Perle.Cast.Proxy
{
    /// <summary>
    /// Reads url from start to the end through consecutive range requests of chunkSize bytes,
    /// so that no connection stays idle for long while the reader waits for the TV to consume data.
    /// A chunk which fails is requested again once from where it stopped.
    /// </summary>
    public sealed class RangedInputStream : Stream
    {
        public const long DefaultChunkSize = 2L * 1024 * 1024;
        const int RetryDelayMillis = 500;

        readonly IUpstream upstream;
        readonly string url;
        readonly long chunkSize;
        long position;
        long totalLength = -1;
        UpstreamResponse current;
        long currentEnd = -1; // last byte of the current chunk
        bool finished;
        bool closed;

        public RangedInputStream(IUpstream upstream, string url, long start, long chunkSize = DefaultChunkSize)
        {
            this.upstream = upstream; this.url = url; position = start; this.chunkSize = chunkSize;
        }

        public override int Read(byte[] b, int off, int len)
        {
            if (len == 0) return 0;
            bool retried = false;
            while (true)
            {
                if (closed) throw new IOException("Stream closed");
                if (finished) return 0;
                var body = current?.Body;
                if (body == null)
                {
                    if (!OpenNextChunk()) return 0;
                    body = current?.Body;
                    if (body == null) return 0;
                }
                int read;
                try
                {
                    read = body.Read(b, off, len);
                }
                catch (Exception e) when (e is IOException || e is System.Net.Http.HttpRequestException)
                {
                    if (retried) throw;
                    CastLog.W("Upstream read failed at " + position + ", retrying: " + e.Message);
                    retried = true;
                    CloseCurrent();
                    continue;
                }
                if (read <= 0)
                {
                    CloseCurrent();
                    // a server ignoring ranges sent the whole file
                    if (currentEnd == long.MaxValue) finished = true;
                    // the chunk is complete, or the file is: the next chunk will tell
                    else if (position > currentEnd || (totalLength >= 0 && totalLength <= position)) { }
                    // the connection ended before the end of the chunk
                    else if (retried) throw new IOException("Upstream ended early at " + position);
                    else retried = true;
                    continue;
                }
                position += read;
                return read;
            }
        }

        bool OpenNextChunk()
        {
            if (totalLength >= 0 && totalLength <= position)
            {
                finished = true;
                return false;
            }
            long end = position + chunkSize - 1;
            var response = upstream.Open(url, new HttpRange(position, end));
            if (response.Code >= 400 && response.Code != 416)
            {
                // e.g. a 403 now and then from googlevideo: once more before giving up
                response.Dispose();
                CastLog.W("HTTP " + response.Code + " for range " + position + "-" + end + ", retrying");
                Thread.Sleep(RetryDelayMillis);
                response = upstream.Open(url, new HttpRange(position, end));
            }
            switch (response.Code)
            {
                case 206:
                    long total = HttpRange.TotalLengthFromContentRange(response.ContentRange);
                    if (total >= 0) totalLength = total;
                    currentEnd = end;
                    break;
                case 200:
                    // the server ignored the range and sends everything from the start
                    totalLength = response.ContentLength;
                    SkipFully(response.Body, position);
                    currentEnd = long.MaxValue;
                    break;
                case 416:
                    response.Dispose();
                    finished = true;
                    return false;
                default:
                    response.Dispose();
                    throw new IOException("HTTP " + response.Code + " for range " + position + "-" + end);
            }
            current = response;
            return true;
        }

        static void SkipFully(Stream input, long amount)
        {
            if (input == null) return;
            var scratch = new byte[64 * 1024];
            long remaining = amount;
            while (remaining > 0)
            {
                int n = input.Read(scratch, 0, (int)Math.Min(remaining, scratch.Length));
                if (n <= 0) return;
                remaining -= n;
            }
        }

        void CloseCurrent()
        {
            try { current?.Dispose(); } catch (IOException) { /* nothing to do */ }
            current = null;
        }

        protected override void Dispose(bool disposing)
        {
            closed = true;
            CloseCurrent();
            base.Dispose(disposing);
        }

        /// <summary>Downloads the bytes from start to end (both included).</summary>
        public static byte[] ReadRange(IUpstream upstream, string url, long start, long end)
        {
            using (var response = upstream.Open(url, new HttpRange(start, end)))
            {
                if (response.Code != 206 && response.Code != 200) throw new IOException("HTTP " + response.Code + " for range " + start + "-" + end);
                var body = response.Body ?? throw new IOException("No body for range " + start + "-" + end);
                if (response.Code == 200) SkipFully(body, start);
                var result = new byte[end - start + 1];
                int offset = 0;
                while (offset < result.Length)
                {
                    int read = body.Read(result, offset, result.Length - offset);
                    if (read <= 0) throw new IOException("Range " + start + "-" + end + " ended after " + offset + " bytes");
                    offset += read;
                }
                return result;
            }
        }

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => position; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
