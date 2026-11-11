// SPDX-License-Identifier: GPL-3.0-or-later
#include "TitleCleaner.h"

#include <cwctype>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace perle {
namespace {

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool IsKeyword(const std::wstring& token)
{
    static const wchar_t* keywords[] = {
        L"official", L"lyric", L"lyrics", L"audio", L"video", L"hd", L"hq", L"4k", L"1080p", L"720p",
        L"visualizer", L"remaster", L"remastered", L"clip", L"mv", L"music"
    };
    for (const wchar_t* k : keywords)
        if (token == k) return true;
    return false;
}

// True when the bracketed text contains a keyword as a whole word ("(Official Video)", "[Lyrics]").
bool IsNoiseGroup(const std::wstring& inner)
{
    std::wstring word;
    std::wstring lower = Lower(inner);
    for (size_t i = 0; i <= lower.size(); i++)
    {
        wchar_t c = i < lower.size() ? lower[i] : L' ';
        if (std::iswalnum(c) || c == L'+') word += c;
        else
        {
            if (!word.empty() && IsKeyword(word)) return true;
            word.clear();
        }
    }
    return false;
}

std::vector<std::wstring> Split(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s)
    {
        if (c == L' ' || c == L'\t') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::wstring Join(const std::vector<std::wstring>& words)
{
    std::wstring out;
    for (size_t i = 0; i < words.size(); i++)
    {
        if (i > 0) out += L' ';
        out += words[i];
    }
    return out;
}

// Trims spaces and the separators that remain at the ends, e.g. "Song -" or "| Song".
std::wstring Trim(const std::wstring& s)
{
    const std::wstring junk = L" \t-|:\u00b7/";
    size_t b = s.find_first_not_of(junk);
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(junk);
    return s.substr(b, e - b + 1);
}

}  // namespace

std::wstring CleanTitle(const std::wstring& title)
{
    // 1. bracketed groups that describe the upload, not the song
    std::wstring noBrackets;
    for (size_t i = 0; i < title.size();)
    {
        wchar_t open = title[i];
        wchar_t close = open == L'(' ? L')' : open == L'[' ? L']' : open == L'{' ? L'}' : 0;
        if (close != 0)
        {
            size_t end = title.find(close, i + 1);
            if (end != std::wstring::npos)
            {
                if (IsNoiseGroup(title.substr(i + 1, end - i - 1))) { i = end + 1; continue; }
            }
        }
        noBrackets += title[i];
        i++;
    }

    // 2. quality words anywhere ("HD", "4K") and repeated spaces
    std::vector<std::wstring> kept;
    for (const auto& word : Split(noBrackets))
    {
        std::wstring w = Lower(word);
        if (w == L"hd" || w == L"hq" || w == L"4k" || w == L"1080p" || w == L"720p") continue;
        kept.push_back(word);
    }
    return Trim(Join(kept));
}

std::wstring ArtistOf(const std::wstring& channel, const std::wstring& title)
{
    std::wstring cleaned = CleanTitle(title);
    size_t dash = cleaned.find(L" - ");
    if (dash != std::wstring::npos)
    {
        std::wstring artist = Trim(cleaned.substr(0, dash));
        if (!artist.empty()) return artist;
    }
    std::wstring name = Trim(channel);
    const std::wstring topic = L" - Topic";
    if (name.size() > topic.size() && name.compare(name.size() - topic.size(), topic.size(), topic) == 0)
        name = Trim(name.substr(0, name.size() - topic.size()));
    const std::wstring vevo = L"VEVO";
    if (name.size() > vevo.size() && name.compare(name.size() - vevo.size(), vevo.size(), vevo) == 0)
        name = Trim(name.substr(0, name.size() - vevo.size()));
    return name;
}

std::wstring TrackOf(const std::wstring& channel, const std::wstring& title)
{
    (void)channel;
    std::wstring cleaned = CleanTitle(title);
    size_t dash = cleaned.find(L" - ");
    if (dash != std::wstring::npos)
    {
        std::wstring track = Trim(cleaned.substr(dash + 3));
        if (!track.empty()) return track;
    }
    return cleaned;
}

std::wstring SafeFileName(const std::wstring& name)
{
    std::wstring out = name;
    for (auto& c : out)
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    out = Trim(out);
    if (out.empty()) out = L"_";
    return out;
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
#else
    std::string out;
    for (wchar_t c : text) out += static_cast<char>(c);
    return out;
#endif
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return std::wstring();
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n);
    return out;
#else
    std::wstring out;
    for (char c : text) out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return out;
#endif
}

}  // namespace perle
