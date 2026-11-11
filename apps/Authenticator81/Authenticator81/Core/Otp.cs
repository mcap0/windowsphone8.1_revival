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
            for (int i = 7; i >= 0; i--)
            {
                msg[i] = (byte)(counter & 0xff);
                counter >>= 8;
            }

            byte[] hash = hmac(alg, key, msg);
            int offset = hash[hash.Length - 1] & 0x0f;
            int binary = ((hash[offset] & 0x7f) << 24) | (hash[offset + 1] << 16)
                | (hash[offset + 2] << 8) | hash[offset + 3];

            long modulus = 1;
            for (int i = 0; i < digits; i++)
                modulus *= 10;

            return (binary % modulus).ToString(CultureInfo.InvariantCulture).PadLeft(digits, '0');
        }

        public static long TotpStep(long unixSeconds, int period)
        {
            return unixSeconds / period;
        }

        public static string Totp(Func<OtpAlgorithm, byte[], byte[], byte[]> hmac, OtpAccount account, long unixSeconds)
        {
            return Hotp(hmac, account.Algorithm, account.Secret, TotpStep(unixSeconds, account.Period), account.Digits);
        }

        public static int SecondsLeft(long unixSeconds, int period)
        {
            return period - (int)(unixSeconds % period);
        }
    }

    public static class Base32
    {
        const string Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

        public static byte[] Decode(string text)
        {
            var bytes = new List<byte>();
            int buffer = 0;
            int bits = 0;

            foreach (char raw in text)
            {
                char value = char.ToUpperInvariant(raw);
                if (value == '=' || value == ' ' || value == '-')
                    continue;

                int index = Alphabet.IndexOf(value);
                if (index < 0)
                    throw new FormatException("Carattere non valido nel segreto: " + raw);

                buffer = (buffer << 5) | index;
                bits += 5;
                if (bits >= 8)
                {
                    bits -= 8;
                    bytes.Add((byte)(buffer >> bits));
                    buffer &= (1 << bits) - 1;
                }
            }

            return bytes.ToArray();
        }

        public static string Encode(byte[] data)
        {
            var builder = new System.Text.StringBuilder();
            int buffer = 0;
            int bits = 0;

            foreach (byte value in data)
            {
                buffer = (buffer << 8) | value;
                bits += 8;
                while (bits >= 5)
                {
                    bits -= 5;
                    builder.Append(Alphabet[(buffer >> bits) & 31]);
                }
                buffer &= (1 << bits) - 1;
            }

            if (bits > 0)
                builder.Append(Alphabet[(buffer << (5 - bits)) & 31]);

            return builder.ToString();
        }
    }
}
