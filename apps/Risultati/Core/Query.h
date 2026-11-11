#pragma once
// Pure helpers (no WinRT) so they can be tested with g++: Core/tests.
#include <cctype>
#include <string>
#include <vector>

namespace RisultatiCore
{
    inline int HexValue(wchar_t c)
    {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }

    // Percent-decoding of a query component ('+' = space), bytes read as UTF-8.
    inline std::wstring UrlDecode(const std::wstring& s)
    {
        std::string bytes;
        for (size_t i = 0; i < s.size(); ++i)
        {
            wchar_t c = s[i];
            if (c == L'+') bytes += ' ';
            else if (c == L'%' && i + 2 < s.size() && HexValue(s[i + 1]) >= 0 && HexValue(s[i + 2]) >= 0)
            {
                bytes += (char)(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2]));
                i += 2;
            }
            else if (c < 0x80) bytes += (char)c;
            else
            {
                // already decoded non-ASCII: re-encode as UTF-8
                unsigned v = c;
                if (v < 0x800) { bytes += (char)(0xC0 | (v >> 6)); bytes += (char)(0x80 | (v & 63)); }
                else { bytes += (char)(0xE0 | (v >> 12)); bytes += (char)(0x80 | ((v >> 6) & 63)); bytes += (char)(0x80 | (v & 63)); }
            }
        }
        std::wstring out;
        for (size_t i = 0; i < bytes.size();)
        {
            unsigned char b = bytes[i];
            unsigned v; size_t n;
            if (b < 0x80) { v = b; n = 1; }
            else if ((b & 0xE0) == 0xC0) { v = b & 31; n = 2; }
            else if ((b & 0xF0) == 0xE0) { v = b & 15; n = 3; }
            else if ((b & 0xF8) == 0xF0) { v = b & 7; n = 4; }
            else { out += L'?'; ++i; continue; }
            if (i + n > bytes.size()) { out += L'?'; break; }
            for (size_t k = 1; k < n; ++k) v = (v << 6) | (bytes[i + k] & 63);
            if (v >= 0x10000) { v -= 0x10000; out += (wchar_t)(0xD800 + (v >> 10)); out += (wchar_t)(0xDC00 + (v & 0x3FF)); }
            else out += (wchar_t)v;
            i += n;
        }
        return out;
    }

    // UTF-8 percent-encoding for a query component.
    inline std::wstring UrlEncode(const std::wstring& s)
    {
        std::string bytes;
        for (size_t i = 0; i < s.size(); ++i)
        {
            unsigned v = s[i];
            if (v >= 0xD800 && v < 0xDC00 && i + 1 < s.size()) { v = 0x10000 + ((v - 0xD800) << 10) + (s[i + 1] - 0xDC00); ++i; }
            if (v < 0x80) bytes += (char)v;
            else if (v < 0x800) { bytes += (char)(0xC0 | (v >> 6)); bytes += (char)(0x80 | (v & 63)); }
            else if (v < 0x10000) { bytes += (char)(0xE0 | (v >> 12)); bytes += (char)(0x80 | ((v >> 6) & 63)); bytes += (char)(0x80 | (v & 63)); }
            else { bytes += (char)(0xF0 | (v >> 18)); bytes += (char)(0x80 | ((v >> 12) & 63)); bytes += (char)(0x80 | ((v >> 6) & 63)); bytes += (char)(0x80 | (v & 63)); }
        }
        static const char* hex = "0123456789ABCDEF";
        std::wstring out;
        for (unsigned char b : bytes)
        {
            if (isalnum(b) || b == '-' || b == '_' || b == '.' || b == '~') out += (wchar_t)b;
            else if (b == ' ') out += L'+';
            else { out += L'%'; out += (wchar_t)hex[b >> 4]; out += (wchar_t)hex[b & 15]; }
        }
        return out;
    }

    // Value of parameter `name` in the query string of `url` ("" if missing).
    inline bool QueryParam(const std::wstring& url, const std::wstring& name, std::wstring& value)
    {
        size_t q = url.find(L'?');
        if (q == std::wstring::npos) return false;
        size_t end = url.find(L'#', q);
        std::wstring query = url.substr(q + 1, end == std::wstring::npos ? std::wstring::npos : end - q - 1);
        size_t pos = 0;
        while (pos <= query.size())
        {
            size_t amp = query.find(L'&', pos);
            std::wstring part = query.substr(pos, amp == std::wstring::npos ? std::wstring::npos : amp - pos);
            size_t eq = part.find(L'=');
            if (part.substr(0, eq) == name)
            {
                value = eq == std::wstring::npos ? L"" : UrlDecode(part.substr(eq + 1));
                return true;
            }
            if (amp == std::wstring::npos) break;
            pos = amp + 1;
        }
        return false;
    }

    // Launch arguments from SearchCM (search URL), the risultati: protocol or a plain query.
    // Returns the search text; `url` is set when the argument is a non-search web address.
    inline std::wstring QueryFromArgs(const std::wstring& args, std::wstring& url)
    {
        url.clear();
        std::wstring a = args;
        if (a.compare(0, 10, L"risultati:") == 0)
        {
            a = a.substr(10);
            while (!a.empty() && a[0] == L'/') a = a.substr(1);
            if (a.compare(0, 2, L"q=") == 0 || a.compare(0, 1, L"?") == 0) a = L"x:/?" + (a[0] == L'?' ? a.substr(1) : a);
            else return UrlDecode(a);
        }
        std::wstring v;
        if (QueryParam(a, L"q", v)) return v;
        if (a.compare(0, 7, L"http://") == 0 || a.compare(0, 8, L"https://") == 0)
        {
            url = a;
            return L"";
        }
        return a;
    }

    inline std::wstring GoogleUrl(const std::wstring& query)
    {
        return L"https://www.google.com/search?hl=it&q=" + UrlEncode(query);
    }

    // Display form of a result address: host + path, without scheme and "www.".
    inline std::wstring ShortUrl(const std::wstring& url)
    {
        std::wstring s = url;
        size_t p = s.find(L"://");
        if (p != std::wstring::npos) s = s.substr(p + 3);
        if (s.compare(0, 4, L"www.") == 0) s = s.substr(4);
        size_t q = s.find_first_of(L"?#");
        if (q != std::wstring::npos) s = s.substr(0, q);
        if (!s.empty() && s.back() == L'/') s.pop_back();
        if (s.size() > 60) s = s.substr(0, 57) + L"...";
        return s;
    }

    // Bing image search (Google Images refuses plain requests: "browser non supportato").
    inline std::wstring BingImagesUrl(const std::wstring& query)
    {
        return L"https://www.bing.com/images/search?setlang=it&q=" + UrlEncode(query);
    }

    // The HTML entities found in attribute values: &quot; &amp; &lt; &gt; &#39; &#NN; &#xNN;
    inline std::wstring HtmlUnescape(const std::wstring& s)
    {
        std::wstring out;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] != L'&') { out += s[i]; continue; }
            size_t semi = s.find(L';', i);
            if (semi == std::wstring::npos || semi - i > 10) { out += s[i]; continue; }
            std::wstring e = s.substr(i + 1, semi - i - 1);
            wchar_t c = 0;
            if (e == L"quot") c = L'"';
            else if (e == L"amp") c = L'&';
            else if (e == L"lt") c = L'<';
            else if (e == L"gt") c = L'>';
            else if (e == L"apos") c = L'\'';
            else if (e.size() > 1 && e[0] == L'#')
            {
                unsigned v = 0;
                bool hex = e[1] == L'x' || e[1] == L'X';
                for (size_t k = hex ? 2 : 1; k < e.size(); ++k)
                {
                    int d = hex ? HexValue(e[k]) : (e[k] >= L'0' && e[k] <= L'9' ? e[k] - L'0' : -1);
                    if (d < 0) { v = 0; break; }
                    v = v * (hex ? 16 : 10) + d;
                }
                c = (wchar_t)v;
            }
            if (c == 0) { out += s[i]; continue; }
            out += c;
            i = semi;
        }
        return out;
    }

    // Values of every attribute `name="..."` (unescaped), e.g. m="{...}" of the Bing image tiles.
    inline std::vector<std::wstring> AttributeValues(const std::wstring& html, const std::wstring& name, size_t max)
    {
        std::vector<std::wstring> out;
        std::wstring key = L" " + name + L"=\"";
        for (size_t p = html.find(key); p != std::wstring::npos && out.size() < max; p = html.find(key, p))
        {
            p += key.size();
            size_t e = html.find(L'"', p);
            if (e == std::wstring::npos) break;
            out.push_back(HtmlUnescape(html.substr(p, e - p)));
            p = e;
        }
        return out;
    }
}
