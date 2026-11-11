using System;
using System.Collections.Generic;
using System.Globalization;

namespace Authenticator.Core
{
    public enum OtpType { Totp, Hotp }

    public enum OtpAlgorithm { Sha1, Sha256, Sha512 }

    /// <summary>One account, as described by an otpauth:// URI.</summary>
    public sealed class OtpAccount
    {
        public string Id { get; set; } = Guid.NewGuid().ToString("N");
        public OtpType Type { get; set; } = OtpType.Totp;
        public string Issuer { get; set; } = "";
        public string Label { get; set; } = "";
        public byte[] Secret { get; set; } = new byte[0];
        public OtpAlgorithm Algorithm { get; set; } = OtpAlgorithm.Sha1;
        public int Digits { get; set; } = 6;
        public int Period { get; set; } = 30;
        public long Counter { get; set; }
    }

    /// <summary>RFC 4226 (HOTP) and RFC 6238 (TOTP), with the HMAC passed in so this file has no platform dependency.</summary>
    public static class Otp
    {
        public static string Hotp(Func<OtpAlgorithm, byte[], byte[], byte[]> hmac, OtpAlgorithm alg, byte[] key, long counter, int digits)
        {
            var msg = new byte[8];
            for (int i = 7; i >= 0; i--) { msg[i] = (byte)(counter & 0xff); counter >>= 8; }
            byte[] h = hmac(alg, key, msg);
            int o = h[h.Length - 1] & 0x0f;
            int bin = ((h[o] & 0x7f) << 24) | (h[o + 1] << 16) | (h[o + 2] << 8) | h[o + 3];
            long mod = 1;
            for (int i = 0; i < digits; i++) mod *= 10;
            return (bin % mod).ToString(CultureInfo.InvariantCulture).PadLeft(digits, '0');
        }

        public static long TotpStep(long unixSeconds, int period) => unixSeconds / period;

        public static string Totp(Func<OtpAlgorithm, byte[], byte[], byte[]> hmac, OtpAccount a, long unixSeconds) =>
            Hotp(hmac, a.Algorithm, a.Secret, TotpStep(unixSeconds, a.Period), a.Digits);

        public static int SecondsLeft(long unixSeconds, int period) => period - (int)(unixSeconds % period);
    }

    public static class Base32
    {
        const string Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

        public static byte[] Decode(string s)
        {
            var bytes = new List<byte>();
            int buffer = 0, bits = 0;
            foreach (char raw in s)
            {
                char c = char.ToUpperInvariant(raw);
                if (c == '=' || c == ' ' || c == '-') continue;
                int v = Alphabet.IndexOf(c);
                if (v < 0) throw new FormatException("Carattere non valido nel segreto: " + raw);
                buffer = (buffer << 5) | v;
                bits += 5;
                if (bits >= 8) { bits -= 8; bytes.Add((byte)(buffer >> bits)); buffer &= (1 << bits) - 1; }
            }
            return bytes.ToArray();
        }

        public static string Encode(byte[] data)
        {
            var sb = new System.Text.StringBuilder();
            int buffer = 0, bits = 0;
            foreach (byte b in data)
            {
                buffer = (buffer << 8) | b;
                bits += 8;
                while (bits >= 5) { bits -= 5; sb.Append(Alphabet[(buffer >> bits) & 31]); }
                buffer &= (1 << bits) - 1;
            }
            if (bits > 0) sb.Append(Alphabet[(buffer << (5 - bits)) & 31]);
            return sb.ToString();
        }
    }

    /// <summary>Parses and builds otpauth:// URIs (Google Authenticator key URI format).</summary>
    public static class OtpUri
    {
        public static OtpAccount Parse(string uri)
        {
            uri = uri.Trim();
            const string scheme = "otpauth://";
            if (!uri.StartsWith(scheme, StringComparison.OrdinalIgnoreCase))
                throw new FormatException("Non è un codice otpauth://");
            string rest = uri.Substring(scheme.Length);
            int slash = rest.IndexOf('/');
            if (slash < 0) throw new FormatException("otpauth senza etichetta");
            string type = rest.Substring(0, slash).ToLowerInvariant();
            rest = rest.Substring(slash + 1);
            string query = "";
            int q = rest.IndexOf('?');
            if (q >= 0) { query = rest.Substring(q + 1); rest = rest.Substring(0, q); }

            var a = new OtpAccount();
            if (type == "totp") a.Type = OtpType.Totp;
            else if (type == "hotp") a.Type = OtpType.Hotp;
            else throw new FormatException("Tipo non supportato: " + type);

            string label = Uri.UnescapeDataString(rest);
            int colon = label.IndexOf(':');
            if (colon >= 0) { a.Issuer = label.Substring(0, colon).Trim(); a.Label = label.Substring(colon + 1).Trim(); }
            else a.Label = label.Trim();

            string secret = null;
            foreach (string part in query.Split('&'))
            {
                if (part.Length == 0) continue;
                int eq = part.IndexOf('=');
                string k = (eq < 0 ? part : part.Substring(0, eq)).ToLowerInvariant();
                string v = eq < 0 ? "" : Uri.UnescapeDataString(part.Substring(eq + 1).Replace('+', ' '));
                switch (k)
                {
                    case "secret": secret = v; break;
                    case "issuer": a.Issuer = v; break;
                    case "algorithm":
                        var u = v.ToUpperInvariant();
                        a.Algorithm = u == "SHA256" ? OtpAlgorithm.Sha256 : u == "SHA512" ? OtpAlgorithm.Sha512 : OtpAlgorithm.Sha1;
                        break;
                    case "digits": a.Digits = int.Parse(v, CultureInfo.InvariantCulture); break;
                    case "period": a.Period = int.Parse(v, CultureInfo.InvariantCulture); break;
                    case "counter": a.Counter = long.Parse(v, CultureInfo.InvariantCulture); break;
                }
            }
            if (string.IsNullOrEmpty(secret)) throw new FormatException("Manca il segreto");
            a.Secret = Base32.Decode(secret);
            if (a.Secret.Length == 0) throw new FormatException("Segreto vuoto");
            if (a.Digits < 6 || a.Digits > 8) throw new FormatException("Cifre non valide");
            if (a.Period <= 0) throw new FormatException("Periodo non valido");
            return a;
        }

        public static string Build(OtpAccount a)
        {
            string label = string.IsNullOrEmpty(a.Issuer) ? a.Label : a.Issuer + ":" + a.Label;
            var sb = new System.Text.StringBuilder();
            sb.Append("otpauth://").Append(a.Type == OtpType.Totp ? "totp" : "hotp").Append('/');
            sb.Append(Uri.EscapeDataString(label));
            sb.Append("?secret=").Append(Base32.Encode(a.Secret));
            if (!string.IsNullOrEmpty(a.Issuer)) sb.Append("&issuer=").Append(Uri.EscapeDataString(a.Issuer));
            if (a.Algorithm != OtpAlgorithm.Sha1) sb.Append("&algorithm=").Append(a.Algorithm.ToString().ToUpperInvariant());
            if (a.Digits != 6) sb.Append("&digits=").Append(a.Digits);
            if (a.Type == OtpType.Totp && a.Period != 30) sb.Append("&period=").Append(a.Period);
            if (a.Type == OtpType.Hotp) sb.Append("&counter=").Append(a.Counter);
            return sb.ToString();
        }
    }
}
