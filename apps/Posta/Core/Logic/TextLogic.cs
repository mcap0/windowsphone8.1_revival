using System;
using System.Globalization;
using System.Text.RegularExpressions;
using Posta.Core.Model;

namespace Posta.Core.Logic
{
    /// <summary>Reply subject, quoting and address helpers. Pure: no I/O, no platform APIs.</summary>
    public static class ReplyText
    {
        private static readonly Regex RePrefix = new Regex(@"^\s*re(\[\d+\])?\s*:", RegexOptions.IgnoreCase);
        private static readonly Regex FwdPrefix = new Regex(@"^\s*(fwd?|inoltra)\s*:", RegexOptions.IgnoreCase);

        public static string ReplySubject(string subject)
        {
            var value = (subject ?? "").Trim();
            if (RePrefix.IsMatch(value))
                return value;
            return "Re: " + value;
        }

        public static string ForwardSubject(string subject)
        {
            var value = (subject ?? "").Trim();
            if (FwdPrefix.IsMatch(value))
                return value;
            return "Fwd: " + value;
        }

        /// <summary>Extracts the bare address from "Name &lt;addr@x&gt;" (or returns the input trimmed).</summary>
        public static string ExtractAddress(string from)
        {
            var value = (from ?? "").Trim();
            var match = Regex.Match(value, "<([^>]+)>");
            if (match.Success)
                return match.Groups[1].Value.Trim();
            return value;
        }

        /// <summary>
        /// Quoted block placed under the cursor when replying. Starts with blank lines so the
        /// caller can append it to an empty body.
        /// </summary>
        public static string Quote(string originalText, string from, string dateText)
        {
            var header = string.Format(CultureInfo.InvariantCulture,
                "Il {0}, {1} ha scritto:", dateText, from ?? "");
            var builder = new System.Text.StringBuilder();
            builder.Append("\r\n\r\n");
            builder.Append(header);
            builder.Append("\r\n");

            var lines = (originalText ?? "").Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
            foreach (var line in lines)
            {
                builder.Append(line.Length == 0 ? ">" : "> " + line);
                builder.Append("\r\n");
            }

            return builder.ToString();
        }

        /// <summary>Rough plain text of an HTML body (used only to quote HTML-only messages).</summary>
        public static string PlainFromHtml(string html)
        {
            var value = html ?? "";
            value = Regex.Replace(value, @"<(script|style|head)\b.*?</\1\s*>", " ",
                RegexOptions.IgnoreCase | RegexOptions.Singleline);
            value = Regex.Replace(value, @"<br\s*/?>", "\n", RegexOptions.IgnoreCase);
            value = Regex.Replace(value, @"</(p|div|tr|li|h[1-6])\s*>", "\n",
                RegexOptions.IgnoreCase);
            value = Regex.Replace(value, "<[^>]+>", "");
            value = value.Replace("&nbsp;", " ").Replace("&lt;", "<").Replace("&gt;", ">")
                .Replace("&quot;", "\"").Replace("&#39;", "'").Replace("&apos;", "'")
                .Replace("&amp;", "&");
            value = Regex.Replace(value, @"[ \t]+", " ");
            value = Regex.Replace(value, @"\n\s*\n\s*\n+", "\n\n");
            return value.Trim();
        }

        /// <summary>References header for a reply: the original References followed by its Message-ID.</summary>
        public static string ReferencesFor(MailContent original)
        {
            var parts = new System.Collections.Generic.List<string>();
            if (!string.IsNullOrWhiteSpace(original.References))
                parts.Add(original.References.Trim());
            if (!string.IsNullOrWhiteSpace(original.MessageId))
                parts.Add(original.MessageId.Trim());
            return string.Join(" ", parts);
        }
    }

    public enum BodyKind
    {
        Empty,
        Plain,
        Html
    }

    public sealed class BodyChoice
    {
        public BodyKind Kind { get; set; }
        public string Text { get; set; } = "";
    }

    /// <summary>Prefers text/plain; falls back to HTML only when there is no usable plain text.</summary>
    public static class BodySelector
    {
        public static BodyChoice Choose(string plain, string html)
        {
            if (!string.IsNullOrWhiteSpace(plain))
                return new BodyChoice { Kind = BodyKind.Plain, Text = plain };
            if (!string.IsNullOrWhiteSpace(html))
                return new BodyChoice { Kind = BodyKind.Html, Text = html };
            return new BodyChoice { Kind = BodyKind.Empty, Text = "" };
        }
    }

    /// <summary>
    /// Makes third-party HTML safe-ish for a WebView: removes scripts, frames and inline handlers,
    /// and blocks remote images unless explicitly allowed. Regex based, so it is a best effort.
    /// </summary>
    public static class HtmlPreparer
    {
        private static readonly RegexOptions Ci = RegexOptions.IgnoreCase | RegexOptions.Singleline;

        public static int CountRemoteImages(string html)
        {
            return Regex.Matches(html ?? "",
                @"<img\b[^>]*?\bsrc\s*=\s*[""']?\s*https?:", Ci).Count;
        }

        public static string Prepare(string html, bool allowRemoteImages)
        {
            return Prepare(html, allowRemoteImages, false);
        }

        public static string Prepare(string html, bool allowRemoteImages, bool darkTheme)
        {
            var value = html ?? "";
            value = Regex.Replace(value, @"<!doctype[^>]*>", "", Ci);
            value = Regex.Replace(value, @"<head\b.*?</head\s*>", "", Ci);
            value = Regex.Replace(value, @"<script\b.*?</script\s*>", "", Ci);
            value = Regex.Replace(value, @"<script\b[^>]*/?>", "", Ci);
            value = Regex.Replace(value,
                @"</?(html|body|iframe|frame|frameset|object|embed|link|meta|base|form)\b[^>]*>", "", Ci);
            value = Regex.Replace(value,
                @"\son[a-z]+\s*=\s*(""[^""]*""|'[^']*'|[^\s>]+)", "", Ci);
            value = Regex.Replace(value,
                @"(href|src)\s*=\s*([""'])\s*javascript:[^""']*\2", "$1=$2#$2", Ci);

            if (!allowRemoteImages)
            {
                value = Regex.Replace(value,
                    @"(<img\b[^>]*?)\bsrc(\s*=\s*(""\s*https?:[^""]*""|'\s*https?:[^']*'|https?:[^\s>]*))",
                    "$1data-blocked-src$2", Ci);
                value = Regex.Replace(value, @"url\(\s*(['""]?)\s*https?:[^)]*\)", "none", Ci);
            }

            var foreground = darkTheme ? "#f3f3f3" : "#202020";
            var background = darkTheme ? "#000000" : "#ffffff";
            var link = darkTheme ? "#7db7ff" : "#0067c0";
            var head =
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">" +
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">" +
                "<style>html,body{background:" + background + ";color:" + foreground + ";}" +
                "body{font-family:'Segoe WP',sans-serif;font-size:18px;margin:8px;" +
                "word-wrap:break-word;}a{color:" + link + ";}" +
                "img{max-width:100%;height:auto;}pre{white-space:pre-wrap;}" +
                "table{max-width:100%;}td,th{word-wrap:break-word;}</style></head><body>";

            return head + value + LinkScript + "</body></html>";
        }

        // Added after sanitising: a tap on a link -> window.external.notify("open:<href>"), a long press ->
        // "menu:<href>" (MessagePage opens it in the browser or offers to copy it). No inline handlers on purpose.
        public const string LinkScript =
            "<script>(function(){var timer=null,held=false,x=0,y=0;" +
            "function linkOf(n){while(n&&n.tagName!=='A')n=n.parentNode;return n&&n.getAttribute&&n.getAttribute('href')?n:null;}" +
            "function cancel(){if(timer){clearTimeout(timer);timer=null;}}" +
            "document.addEventListener('pointerdown',function(e){cancel();held=false;var a=linkOf(e.target);if(!a)return;" +
            "x=e.clientX;y=e.clientY;timer=setTimeout(function(){timer=null;held=true;window.external.notify('menu:'+a.href);},600);},true);" +
            "document.addEventListener('pointermove',function(e){if(Math.abs(e.clientX-x)+Math.abs(e.clientY-y)>12)cancel();},true);" +
            "document.addEventListener('pointerup',cancel,true);document.addEventListener('pointercancel',cancel,true);" +
            "document.addEventListener('click',function(e){var a=linkOf(e.target);if(!a)return;e.preventDefault();" +
            "if(held){held=false;return;}window.external.notify('open:'+a.href);},true);})();</script>";

        private static readonly Regex UrlRx = new Regex(@"\b(https?://|www\.)[^\s<>""']+", RegexOptions.IgnoreCase);

        /// <summary>Plain-text body as HTML with clickable links (same view and link handling as HTML mail).</summary>
        public static string PlainToHtml(string text)
        {
            var escaped = (text ?? "").Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");
            var linked = UrlRx.Replace(escaped, m =>
            {
                var shown = m.Value.TrimEnd('.', ',', ';', ':', ')', '!', '?');
                var tail = m.Value.Substring(shown.Length);
                var href = shown.StartsWith("www.", StringComparison.OrdinalIgnoreCase) ? "http://" + shown : shown;
                return "<a href=\"" + href.Replace("\"", "%22") + "\">" + shown + "</a>" + tail;
            });
            return "<div style=\"white-space:pre-wrap\">" + linked + "</div>";
        }
    }

    /// <summary>Input checks for the account screen. Messages are in Italian for the UI.</summary>
    public static class AccountValidator
    {
        private static readonly Regex EmailRx =
            new Regex(@"^[^@\s]+@[^@\s]+\.[^@\s]+$");

        public static string[] Validate(AccountSettings settings, string password, bool passwordRequired)
        {
            var errors = new System.Collections.Generic.List<string>();
            if (settings == null || !EmailRx.IsMatch((settings.Email ?? "").Trim()))
                errors.Add("Indirizzo e-mail non valido.");
            if (settings == null || string.IsNullOrWhiteSpace(settings.ImapHost))
                errors.Add("Host IMAP mancante.");
            if (settings == null || string.IsNullOrWhiteSpace(settings.SmtpHost))
                errors.Add("Host SMTP mancante.");
            if (settings != null && (settings.ImapPort < 1 || settings.ImapPort > 65535))
                errors.Add("Porta IMAP non valida.");
            if (settings != null && (settings.SmtpPort < 1 || settings.SmtpPort > 65535))
                errors.Add("Porta SMTP non valida.");
            if (passwordRequired && string.IsNullOrEmpty(password))
                errors.Add("Inserisci la password per le app.");
            return errors.ToArray();
        }
    }
}
