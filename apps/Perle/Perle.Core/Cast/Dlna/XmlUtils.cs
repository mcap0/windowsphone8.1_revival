/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.XmlUtils, with System.Xml instead of the JDK DOM parser.
 */

using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml;

namespace Perle.Cast.Dlna
{
    /// <summary>Small helpers to write and read the XML documents used by UPnP.</summary>
    public static class XmlUtils
    {
        const char ByteOrderMark = '﻿';

        /// <summary>
        /// Escapes a string so that it can be put in XML text or in an attribute value.
        ///
        /// Characters which are not allowed in XML 1.0 at all (most control characters and unpaired
        /// surrogates) are removed, since a single one of them makes the whole document invalid and
        /// TVs reject it. Emojis and other characters outside the BMP are kept as they are: they are
        /// valid XML and the document is sent as UTF-8.
        /// </summary>
        public static string Escape(string text)
        {
            var result = new StringBuilder(text.Length + 16);
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (c == '&') result.Append("&amp;");
                else if (c == '<') result.Append("&lt;");
                else if (c == '>') result.Append("&gt;");
                else if (c == '"') result.Append("&quot;");
                else if (c == '\'') result.Append("&apos;");
                else if (char.IsHighSurrogate(c))
                {
                    if (i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
                    {
                        result.Append(c).Append(text[i + 1]);
                        i++;
                    }
                    // an unpaired high surrogate is dropped
                }
                else if (char.IsLowSurrogate(c)) { } // unpaired low surrogate, dropped
                else if (IsValidXmlChar(c)) result.Append(c);
                // else: invalid control character, dropped
            }
            return result.ToString();
        }

        static bool IsValidXmlChar(char c) =>
            c == '\t' || c == '\n' || c == '\r' || (c >= ' ' && c <= '퟿') || (c >= '' && c <= '�');

        /// <summary>
        /// Parses an XML document in a namespace aware way, with DTDs and external entities disabled
        /// (the documents come from devices on the local network and must not be able to read files).
        /// </summary>
        public static XmlDocument Parse(string xml)
        {
            // some devices send a BOM or whitespace before the XML declaration
            var cleaned = xml.TrimStart(ByteOrderMark, ' ', '\n', '\r', '\t');
            var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
            // like the JDK DOM parser, keep whitespace text (e.g. between two spans of a subtitle)
            var doc = new XmlDocument { XmlResolver = null, PreserveWhitespace = true };
            using (var reader = XmlReader.Create(new StringReader(cleaned), settings)) doc.Load(reader);
            return doc;
        }

        /// <returns>the direct child elements of parent with the given local name</returns>
        public static List<XmlElement> Children(XmlNode parent, string localName) =>
            parent.ChildNodes.OfType<XmlElement>().Where(e => LocalNameOf(e) == localName).ToList();

        /// <returns>the first direct child element of parent with the given local name</returns>
        public static XmlElement Child(XmlNode parent, string localName) => Children(parent, localName).FirstOrDefault();

        /// <returns>the trimmed text of the first direct child with the given local name</returns>
        public static string ChildText(XmlNode parent, string localName) => Child(parent, localName)?.InnerText?.Trim();

        /// <returns>the first element with the given local name anywhere below parent</returns>
        public static XmlElement FindFirst(XmlNode parent, string localName)
        {
            foreach (var e in parent.ChildNodes.OfType<XmlElement>())
            {
                if (LocalNameOf(e) == localName) return e;
                var found = FindFirst(e, localName);
                if (found != null) return found;
            }
            return null;
        }

        static string LocalNameOf(XmlElement e) => !string.IsNullOrEmpty(e.LocalName) ? e.LocalName : e.Name.Substring(e.Name.IndexOf(':') + 1);
    }
}
