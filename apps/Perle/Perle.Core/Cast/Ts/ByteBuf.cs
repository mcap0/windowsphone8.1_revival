/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The subset of java.nio.ByteBuffer (big-endian) used by the Cast code, so that the port stays
 * line by line close to the Kotlin original.
 */

using System;
using System.IO;

namespace Perle.Cast.Ts
{
    public sealed class ByteBuf
    {
        readonly byte[] array;
        readonly int offset; // start of this view in array
        int position, limit;

        ByteBuf(byte[] array, int offset, int limit) { this.array = array; this.offset = offset; this.limit = limit; }

        public static ByteBuf Wrap(byte[] data) => new ByteBuf(data, 0, data.Length);

        public int Position() => position;
        public void Position(int p) { if (p < 0 || p > limit) throw new IOException("Invalid position " + p); position = p; }
        public int Limit() => limit;
        public void Limit(int l) { if (l < 0 || offset + l > array.Length) throw new IOException("Invalid limit " + l); limit = l; if (position > l) position = l; }
        public int Remaining() => limit - position;
        public bool HasRemaining() => position < limit;

        /// <summary>A view from the position to the limit, with its own position 0.</summary>
        public ByteBuf Slice() => new ByteBuf(array, offset + position, limit - position);

        public ByteBuf Duplicate() => new ByteBuf(array, offset, limit) { position = position };

        void Need(int at, int n) { if (at < 0 || at + n > limit) throw new EndOfStreamException("Buffer underflow"); }

        public byte Get(int at) { Need(at, 1); return array[offset + at]; }
        public byte Get() { byte b = Get(position); position++; return b; }

        public void Get(byte[] dst) { Need(position, dst.Length); Buffer.BlockCopy(array, offset + position, dst, 0, dst.Length); position += dst.Length; }

        public short GetShort(int at) { Need(at, 2); int i = offset + at; return (short)((array[i] << 8) | array[i + 1]); }
        public short GetShort() { var v = GetShort(position); position += 2; return v; }

        public int GetInt(int at)
        {
            Need(at, 4); int i = offset + at;
            return (array[i] << 24) | (array[i + 1] << 16) | (array[i + 2] << 8) | array[i + 3];
        }
        public int GetInt() { var v = GetInt(position); position += 4; return v; }

        public long GetLong(int at) => ((long)GetInt(at) << 32) | (uint)GetInt(at + 4);
        public long GetLong() { var v = GetLong(position); position += 8; return v; }
    }
}
