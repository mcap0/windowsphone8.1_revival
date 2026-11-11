/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.CastLog.
 */

using System;
using System.Collections.Generic;
using System.Text;

namespace Perle.Cast
{
    /// <summary>
    /// In-memory circular log of everything "Cast to TV" does: SSDP answers, device descriptions,
    /// SOAP requests and responses, proxy requests and errors.
    ///
    /// DLNA renderers differ a lot from each other and the developer usually cannot test on the
    /// user's TV, so the user can copy this log from the settings and attach it to a bug report.
    /// </summary>
    public static class CastLog
    {
        /// <summary>Maximum number of entries kept in memory; the oldest ones are dropped first.</summary>
        public const int MaxEntries = 1000;

        /// <summary>Long payloads (XML documents) are truncated to this many characters.</summary>
        public const int MaxEntryLength = 8000;

        /// <summary>Size of the log copied to be pasted in a message.</summary>
        public const int ShareLength = 20000;

        static readonly LinkedList<string> entries = new LinkedList<string>();
        static readonly object gate = new object();

        /// <summary>Additional destination of every log line, e.g. the debugger. Called outside of the lock.</summary>
        public static Action<char, string, Exception> Sink;

        public static void D(string message) => Add('D', message, null);

        public static void W(string message, Exception error = null) => Add('W', message, error);

        public static void E(string message, Exception error = null) => Add('E', message, error);

        static void Add(char level, string message, Exception error)
        {
            var text = new StringBuilder(Truncate(message));
            if (error != null) text.Append('\n').Append(Truncate(error.ToString()));
            lock (gate)
            {
                entries.AddLast(DateTime.Now.ToString("HH:mm:ss.fff") + " " + level + " " + text);
                while (entries.Count > MaxEntries) entries.RemoveFirst();
            }
            Sink?.Invoke(level, message, error);
        }

        static string Truncate(string s) => s.Length <= MaxEntryLength ? s : s.Substring(0, MaxEntryLength) + "… [" + (s.Length - MaxEntryLength) + " more chars]";

        /// <returns>the whole log, oldest entries first, ready to be copied to the clipboard</returns>
        public static string Dump() { lock (gate) return string.Join("\n", entries); }

        /// <returns>the newest entries, oldest first, at most maxChars characters: what is pasted in
        /// a message is often cut, and the end is what tells what went wrong</returns>
        public static string DumpRecent(int maxChars = ShareLength)
        {
            lock (gate)
            {
                var recent = new List<string>();
                int length = 0;
                for (var node = entries.Last; node != null; node = node.Previous)
                {
                    if (recent.Count > 0 && length + node.Value.Length + 1 > maxChars) break;
                    recent.Add(node.Value);
                    length += node.Value.Length + 1;
                }
                recent.Reverse();
                return string.Join("\n", recent);
            }
        }

        public static int Size() { lock (gate) return entries.Count; }

        public static void Clear() { lock (gate) entries.Clear(); }
    }
}
