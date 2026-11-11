#include "Match.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace MusicaCore
{
    namespace
    {
        wchar_t plain(wchar_t c)
        {
            static const wchar_t from[] = L"\xE0\xE1\xE2\xE3\xE4\xE5\xE8\xE9\xEA\xEB\xEC\xED\xEE\xEF\xF2\xF3\xF4\xF5\xF6\xF9\xFA\xFB\xFC\xE7\xF1";
            static const wchar_t to[]   = L"aaaaaaeeeeiiiiooooouuuucn";
            c = static_cast<wchar_t>(std::towlower(c));
            for (std::size_t i = 0; from[i]; ++i)
                if (c == from[i]) return to[i];
            return c;
        }

        bool vowel(wchar_t c)
        {
            return c == L'a' || c == L'e' || c == L'i' || c == L'o' || c == L'u';
        }
    }

    std::wstring soundKey(const std::wstring& text)
    {
        std::wstring in;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            wchar_t c = plain(text[i]);
            if (std::iswalnum(c)) in += c;
        }
        std::wstring out;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            wchar_t c = in[i];
            const wchar_t next = i + 1 < in.size() ? in[i + 1] : 0;
            switch (c)
            {
            case L'h': continue;
            case L'y': case L'j': c = c == L'y' ? L'i' : L'g'; break;
            case L'w': c = L'u'; break;
            case L'p': if (next == L'h') { c = L'f'; } else c = L'b'; break;
            case L't': c = L'd'; break;
            case L'c': case L'k': case L'q': c = L'g'; break;
            case L'v': c = L'f'; break;
            case L'z': case L'x': c = L's'; break;
            case L'n': c = L'm'; break;
            default: break;
            }
            if (!out.empty() && out[out.size() - 1] == c) continue;
            out += c;
        }
        return out;
    }

    double soundSimilarity(const std::wstring& a, const std::wstring& b)
    {
        if (a.empty() || b.empty()) return 0;
        const double vowelVowel = 0.35, dropVowel = 0.5, dropOther = 1.0, other = 1.0;
        std::vector<double> prev(b.size() + 1), row(b.size() + 1);
        for (std::size_t j = 1; j <= b.size(); ++j)
            prev[j] = prev[j - 1] + (vowel(b[j - 1]) ? dropVowel : dropOther);
        for (std::size_t i = 1; i <= a.size(); ++i)
        {
            row[0] = prev[0] + (vowel(a[i - 1]) ? dropVowel : dropOther);
            for (std::size_t j = 1; j <= b.size(); ++j)
            {
                double swap = a[i - 1] == b[j - 1] ? 0
                    : (vowel(a[i - 1]) && vowel(b[j - 1])) ? vowelVowel : other;
                double fromA = prev[j] + (vowel(a[i - 1]) ? dropVowel : dropOther);
                double fromB = row[j - 1] + (vowel(b[j - 1]) ? dropVowel : dropOther);
                row[j] = (std::min)((std::min)(prev[j - 1] + swap, fromA), fromB);
            }
            prev.swap(row);
        }
        const double longest = static_cast<double>((std::max)(a.size(), b.size()));
        const double score = 1.0 - prev[b.size()] / longest;
        return score < 0 ? 0 : score;
    }

    std::wstring withoutBrackets(const std::wstring& text)
    {
        std::wstring out;
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (c == L'(' || c == L'[') ++depth;
            else if (c == L')' || c == L']') { if (depth > 0) --depth; }
            else if (depth == 0) out += c;
        }
        // nothing outside the brackets: "(Everything I Do)"
        return out.find_first_not_of(L" \"'") == std::wstring::npos ? text : out;
    }

    wchar_t indexLetter(const std::wstring& text)
    {
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = plain(text[i]);
            if (std::iswdigit(c)) return L'#';
            if (std::iswalnum(c)) return static_cast<wchar_t>(std::towupper(c));
        }
        return L'#';
    }
}
