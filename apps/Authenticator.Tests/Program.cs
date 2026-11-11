using System;
using System.Security.Cryptography;
using System.Text;
using Authenticator.Core;

static class Program
{
    static int failures;

    static byte[] Hmac(OtpAlgorithm alg, byte[] key, byte[] msg)
    {
        using (HMAC h = alg == OtpAlgorithm.Sha256 ? new HMACSHA256(key) : alg == OtpAlgorithm.Sha512 ? (HMAC)new HMACSHA512(key) : new HMACSHA1(key))
            return h.ComputeHash(msg);
    }

    static void Check(string got, string want, string what)
    {
        bool ok = got == want;
        Console.WriteLine((ok ? "  ok   " : "  FAIL ") + what + " = " + got + (ok ? "" : " (atteso " + want + ")"));
        if (!ok) failures++;
    }

    static int Main()
    {
        // RFC 4226 appendix D
        var k = Encoding.ASCII.GetBytes("12345678901234567890");
        string[] hotp = { "755224", "287082", "359152", "969429", "338314", "254676", "287922", "162583", "399871", "520489" };
        for (int c = 0; c < hotp.Length; c++) Check(Otp.Hotp(Hmac, OtpAlgorithm.Sha1, k, c, 6), hotp[c], "HOTP counter " + c);

        // RFC 6238 appendix B (8 digits, 30 s)
        var k256 = Encoding.ASCII.GetBytes("12345678901234567890123456789012");
        var k512 = Encoding.ASCII.GetBytes("1234567890123456789012345678901234567890123456789012345678901234");
        long[] times = { 59, 1111111109, 1111111111, 1234567890, 2000000000, 20000000000 };
        string[] sha1 = { "94287082", "07081804", "14050471", "89005924", "69279037", "65353130" };
        string[] sha256 = { "46119246", "68084774", "67062674", "91819424", "90698825", "77737706" };
        string[] sha512 = { "90693936", "25091201", "99943326", "93441116", "38618901", "47863826" };
        for (int i = 0; i < times.Length; i++)
        {
            Check(Otp.Totp(Hmac, new OtpAccount { Secret = k, Digits = 8 }, times[i]), sha1[i], "TOTP SHA1 t=" + times[i]);
            Check(Otp.Totp(Hmac, new OtpAccount { Secret = k256, Digits = 8, Algorithm = OtpAlgorithm.Sha256 }, times[i]), sha256[i], "TOTP SHA256 t=" + times[i]);
            Check(Otp.Totp(Hmac, new OtpAccount { Secret = k512, Digits = 8, Algorithm = OtpAlgorithm.Sha512 }, times[i]), sha512[i], "TOTP SHA512 t=" + times[i]);
        }

        // Base32 and otpauth URIs
        Check(Base32.Encode(Encoding.ASCII.GetBytes("12345678901234567890")), "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", "Base32 encode");
        Check(Encoding.ASCII.GetString(Base32.Decode("gezd gnbv gy3t qojq gezd gnbv gy3t qojq")), "12345678901234567890", "Base32 decode (spazi, minuscole)");
        var a = OtpUri.Parse("otpauth://totp/ACME%20Co:john.doe@email.com?secret=HXDMVJECJJWSRB3HWIZR4IFUGFTMXBOZ&issuer=ACME%20Co&algorithm=SHA1&digits=6&period=30");
        Check(a.Issuer + "|" + a.Label + "|" + a.Digits + "|" + a.Period, "ACME Co|john.doe@email.com|6|30", "otpauth parse");
        var b = OtpUri.Parse(OtpUri.Build(a));
        Check(Base32.Encode(b.Secret) + "|" + b.Issuer + "|" + b.Label, "HXDMVJECJJWSRB3HWIZR4IFUGFTMXBOZ|ACME Co|john.doe@email.com", "otpauth round trip");

        Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        return failures == 0 ? 0 : 1;
    }
}
