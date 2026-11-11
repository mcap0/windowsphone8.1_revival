using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Authenticator.Core
{
    public sealed class OtpImportResult
    {
        public IList<OtpAccount> Accounts { get; private set; }
        public bool IsMigration { get; private set; }
        public int BatchSize { get; private set; }
        public int BatchIndex { get; private set; }

        internal OtpImportResult(IList<OtpAccount> accounts, bool isMigration, int batchSize, int batchIndex)
        {
            Accounts = accounts;
            IsMigration = isMigration;
            BatchSize = batchSize;
            BatchIndex = batchIndex;
        }
    }

    /// <summary>Parses regular authenticator URIs and Google Authenticator migration exports.</summary>
    public static class OtpUri
    {
        public static OtpAccount Parse(string text)
        {
            if (text == null)
                throw new FormatException("Il QR non contiene un URI.");

            string value = text.Trim();
            if (!value.StartsWith("otpauth://", StringComparison.OrdinalIgnoreCase))
                throw new FormatException("Non è un codice otpauth://");

            return ParseOtpAuth(value);
        }

        public static OtpImportResult ParseImport(string text)
        {
            if (text == null)
                throw new FormatException("Il QR non contiene dati.");

            string value = text.Trim();
            if (value.StartsWith("otpauth-migration://", StringComparison.OrdinalIgnoreCase))
                return ParseMigration(value);

            var accounts = new List<OtpAccount>();
            accounts.Add(Parse(value));
            return new OtpImportResult(accounts, false, 1, 0);
        }

        static OtpAccount ParseOtpAuth(string text)
        {
            const string prefix = "otpauth://";
            string rest = text.Substring(prefix.Length);
            int slash = rest.IndexOf('/');
            if (slash < 0)
                throw new FormatException("otpauth senza etichetta.");

            string type = rest.Substring(0, slash).ToLowerInvariant();
            rest = rest.Substring(slash + 1);

            string query = "";
            int question = rest.IndexOf('?');
            if (question >= 0)
            {
                query = rest.Substring(question + 1);
                rest = rest.Substring(0, question);
            }

            int fragment = query.IndexOf('#');
            if (fragment >= 0)
                query = query.Substring(0, fragment);

            var account = new OtpAccount();
            if (type == "totp")
                account.Type = OtpType.Totp;
            else if (type == "hotp")
                account.Type = OtpType.Hotp;
            else
                throw new FormatException("Tipo non supportato: " + type);

            string label = Uri.UnescapeDataString(rest);
            int colon = label.IndexOf(':');
            if (colon >= 0)
            {
                account.Issuer = label.Substring(0, colon).Trim();
                account.Label = label.Substring(colon + 1).Trim();
            }
            else
            {
                account.Label = label.Trim();
            }

            string secret = null;
            string queryIssuer = null;
            foreach (string part in query.Split('&'))
            {
                if (part.Length == 0)
                    continue;

                int equals = part.IndexOf('=');
                string key = (equals < 0 ? part : part.Substring(0, equals)).ToLowerInvariant();
                string encodedValue = equals < 0 ? "" : part.Substring(equals + 1);
                string decodedValue = Uri.UnescapeDataString(encodedValue.Replace('+', ' '));

                switch (key)
                {
                    case "secret":
                        secret = decodedValue;
                        break;
                    case "issuer":
                        queryIssuer = decodedValue.Trim();
                        break;
                    case "algorithm":
                        string algorithm = decodedValue.ToUpperInvariant();
                        if (algorithm == "SHA1")
                            account.Algorithm = OtpAlgorithm.Sha1;
                        else if (algorithm == "SHA256")
                            account.Algorithm = OtpAlgorithm.Sha256;
                        else if (algorithm == "SHA512")
                            account.Algorithm = OtpAlgorithm.Sha512;
                        else
                            throw new FormatException("Algoritmo non supportato: " + decodedValue);
                        break;
                    case "digits":
                        account.Digits = ParseInt(decodedValue, "numero di cifre");
                        break;
                    case "period":
                        account.Period = ParseInt(decodedValue, "periodo");
                        break;
                    case "counter":
                        account.Counter = ParseLong(decodedValue, "contatore");
                        break;
                }
            }

            if (!string.IsNullOrEmpty(queryIssuer))
                account.Issuer = queryIssuer;
            if (string.IsNullOrWhiteSpace(secret))
                throw new FormatException("Manca il segreto.");

            account.Secret = Base32.Decode(secret);
            if (account.Secret.Length == 0)
                throw new FormatException("Segreto vuoto.");
            if (account.Digits != 6 && account.Digits != 8)
                throw new FormatException("Il numero di cifre deve essere 6 o 8.");
            if (account.Period <= 0)
                throw new FormatException("Periodo non valido.");
            if (account.Counter < 0)
                throw new FormatException("Contatore non valido.");

            return account;
        }

        static OtpImportResult ParseMigration(string text)
        {
            const string prefix = "otpauth-migration://";
            string rest = text.Substring(prefix.Length);
            int question = rest.IndexOf('?');
            if (question < 0)
                throw new FormatException("Il QR di migrazione non contiene dati.");

            string path = rest.Substring(0, question).TrimEnd('/');
            if (!string.Equals(path, "offline", StringComparison.OrdinalIgnoreCase))
                throw new FormatException("Tipo di migrazione non supportato.");

            string encodedData = null;
            string query = rest.Substring(question + 1);
            foreach (string part in query.Split('&'))
            {
                int equals = part.IndexOf('=');
                string key = (equals < 0 ? part : part.Substring(0, equals)).ToLowerInvariant();
                if (key == "data")
                {
                    encodedData = equals < 0 ? "" : Uri.UnescapeDataString(part.Substring(equals + 1));
                    break;
                }
            }

            if (string.IsNullOrWhiteSpace(encodedData))
                throw new FormatException("Manca il contenuto del QR di migrazione.");

            byte[] payload = DecodeBase64(encodedData);
            var parsed = ParseMigrationPayload(payload);
            if (parsed.Accounts.Count == 0)
                throw new FormatException("Il QR di migrazione non contiene account.");

            int batchSize = parsed.BatchSize <= 0 ? 1 : parsed.BatchSize;
            if (parsed.BatchIndex < 0 || parsed.BatchIndex >= batchSize)
                throw new FormatException("Indice del QR di migrazione non valido.");

            return new OtpImportResult(parsed.Accounts, true, batchSize, parsed.BatchIndex);
        }

        sealed class MigrationPayload
        {
            public readonly List<OtpAccount> Accounts = new List<OtpAccount>();
            public int BatchSize;
            public int BatchIndex;
        }

        static MigrationPayload ParseMigrationPayload(byte[] data)
        {
            var result = new MigrationPayload();
            int position = 0;

            while (position < data.Length)
            {
                ulong tag = ReadVarint(data, ref position);
                int field = (int)(tag >> 3);
                int wireType = (int)(tag & 7);

                if (field == 0)
                    throw new FormatException("Protobuf non valido.");

                if (field == 1 && wireType == 2)
                {
                    byte[] nested = ReadLengthDelimited(data, ref position);
                    result.Accounts.Add(ParseOtpParameters(nested));
                }
                else if (wireType == 0)
                {
                    ulong value = ReadVarint(data, ref position);
                    if (field == 3)
                        result.BatchSize = SafeInt(value);
                    else if (field == 4)
                        result.BatchIndex = SafeInt(value);
                }
                else
                {
                    SkipField(data, ref position, wireType);
                }
            }

            return result;
        }

        static OtpAccount ParseOtpParameters(byte[] data)
        {
            var account = new OtpAccount();
            string name = "";
            string issuer = "";
            bool hasSecret = false;
            int algorithm = 0;
            int digits = 1;
            int otpType = 2;
            int position = 0;

            while (position < data.Length)
            {
                ulong tag = ReadVarint(data, ref position);
                int field = (int)(tag >> 3);
                int wireType = (int)(tag & 7);

                if (field == 0)
                    throw new FormatException("Protobuf dell'account non valido.");

                if (field == 1 && wireType == 2)
                {
                    account.Secret = ReadLengthDelimited(data, ref position);
                    hasSecret = true;
                }
                else if ((field == 2 || field == 3) && wireType == 2)
                {
                    string value = DecodeUtf8(ReadLengthDelimited(data, ref position));
                    if (field == 2)
                        name = value;
                    else
                        issuer = value;
                }
                else if (wireType == 0)
                {
                    ulong value = ReadVarint(data, ref position);
                    if (field == 4)
                        algorithm = SafeInt(value);
                    else if (field == 5)
                        digits = SafeInt(value);
                    else if (field == 6)
                        otpType = SafeInt(value);
                    else if (field == 7)
                    {
                        if (value > long.MaxValue)
                            throw new FormatException("Contatore non valido.");
                        account.Counter = (long)value;
                    }
                }
                else
                {
                    SkipField(data, ref position, wireType);
                }
            }

            if (!hasSecret || account.Secret.Length == 0)
                throw new FormatException("Un account del QR non contiene un segreto.");

            if (algorithm == 0 || algorithm == 1)
                account.Algorithm = OtpAlgorithm.Sha1;
            else if (algorithm == 2)
                account.Algorithm = OtpAlgorithm.Sha256;
            else if (algorithm == 3)
                account.Algorithm = OtpAlgorithm.Sha512;
            else
                throw new FormatException("Algoritmo di migrazione non supportato.");

            if (digits == 1)
                account.Digits = 6;
            else if (digits == 2)
                account.Digits = 8;
            else
                throw new FormatException("Numero di cifre di migrazione non valido.");

            if (otpType == 1)
                account.Type = OtpType.Hotp;
            else if (otpType == 2)
                account.Type = OtpType.Totp;
            else
                throw new FormatException("Tipo OTP di migrazione non supportato.");

            account.Period = 30;
            account.Issuer = issuer.Trim();
            account.Label = name.Trim();

            if (string.IsNullOrEmpty(account.Issuer))
            {
                int colon = account.Label.IndexOf(':');
                if (colon >= 0)
                {
                    account.Issuer = account.Label.Substring(0, colon).Trim();
                    account.Label = account.Label.Substring(colon + 1).Trim();
                }
            }

            return account;
        }

        static byte[] DecodeBase64(string value)
        {
            string normalized = value.Replace('-', '+').Replace('_', '/');
            normalized = normalized.Replace(" ", "").Replace("\r", "").Replace("\n", "").Replace("\t", "");
            int remainder = normalized.Length % 4;
            if (remainder == 1)
                throw new FormatException("Contenuto base64 non valido.");
            if (remainder == 2)
                normalized += "==";
            else if (remainder == 3)
                normalized += "=";

            try
            {
                return Convert.FromBase64String(normalized);
            }
            catch (FormatException)
            {
                throw new FormatException("Contenuto base64 non valido.");
            }
        }

        static int ParseInt(string value, string description)
        {
            int result;
            if (!int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out result))
                throw new FormatException("Valore non valido per " + description + ".");
            return result;
        }

        static long ParseLong(string value, string description)
        {
            long result;
            if (!long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out result))
                throw new FormatException("Valore non valido per " + description + ".");
            return result;
        }

        static int SafeInt(ulong value)
        {
            if (value > int.MaxValue)
                throw new FormatException("Valore protobuf fuori intervallo.");
            return (int)value;
        }

        static byte[] ReadLengthDelimited(byte[] data, ref int position)
        {
            ulong rawLength = ReadVarint(data, ref position);
            if (rawLength > int.MaxValue || rawLength > (ulong)(data.Length - position))
                throw new FormatException("Lunghezza protobuf non valida.");

            int length = (int)rawLength;
            var result = new byte[length];
            Buffer.BlockCopy(data, position, result, 0, length);
            position += length;
            return result;
        }

        static ulong ReadVarint(byte[] data, ref int position)
        {
            ulong value = 0;
            int shift = 0;

            while (position < data.Length && shift < 64)
            {
                byte current = data[position++];
                value |= ((ulong)(current & 0x7f)) << shift;
                if ((current & 0x80) == 0)
                    return value;
                shift += 7;
            }

            throw new FormatException("Varint protobuf non valido.");
        }

        static void SkipField(byte[] data, ref int position, int wireType)
        {
            if (wireType == 0)
            {
                ReadVarint(data, ref position);
                return;
            }

            if (wireType == 1)
            {
                EnsureAvailable(data, position, 8);
                position += 8;
                return;
            }

            if (wireType == 2)
            {
                ReadLengthDelimited(data, ref position);
                return;
            }

            if (wireType == 5)
            {
                EnsureAvailable(data, position, 4);
                position += 4;
                return;
            }

            throw new FormatException("Tipo protobuf non supportato.");
        }

        static void EnsureAvailable(byte[] data, int position, int count)
        {
            if (position < 0 || count < 0 || position > data.Length - count)
                throw new FormatException("Protobuf troncato.");
        }

        static string DecodeUtf8(byte[] data)
        {
            return Encoding.UTF8.GetString(data, 0, data.Length);
        }

        public static string Build(OtpAccount account)
        {
            string label = string.IsNullOrEmpty(account.Issuer)
                ? account.Label
                : account.Issuer + ":" + account.Label;

            var builder = new StringBuilder();
            builder.Append("otpauth://")
                .Append(account.Type == OtpType.Totp ? "totp" : "hotp")
                .Append('/')
                .Append(Uri.EscapeDataString(label))
                .Append("?secret=")
                .Append(Base32.Encode(account.Secret));

            if (!string.IsNullOrEmpty(account.Issuer))
                builder.Append("&issuer=").Append(Uri.EscapeDataString(account.Issuer));
            if (account.Algorithm != OtpAlgorithm.Sha1)
                builder.Append("&algorithm=").Append(account.Algorithm.ToString().ToUpperInvariant());
            if (account.Digits != 6)
                builder.Append("&digits=").Append(account.Digits.ToString(CultureInfo.InvariantCulture));
            if (account.Type == OtpType.Totp && account.Period != 30)
                builder.Append("&period=").Append(account.Period.ToString(CultureInfo.InvariantCulture));
            if (account.Type == OtpType.Hotp)
                builder.Append("&counter=").Append(account.Counter.ToString(CultureInfo.InvariantCulture));

            return builder.ToString();
        }
    }
}
