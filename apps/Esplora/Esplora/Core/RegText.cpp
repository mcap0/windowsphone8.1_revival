#include "RegText.h"
#include <sstream>
#include <cstring>
#include <cwctype>

namespace RegText
{
    const wchar_t* const HiveNames[5] = { L"HKEY_CLASSES_ROOT", L"HKEY_CURRENT_USER", L"HKEY_LOCAL_MACHINE", L"HKEY_USERS", L"HKEY_CURRENT_CONFIG" };
    const wchar_t* const HiveShort[5] = { L"HKCR", L"HKCU", L"HKLM", L"HKU", L"HKCC" };

    std::wstring LowerCopy(const std::wstring& s)
    {
        std::wstring r = s;
        for (size_t i = 0; i < r.size(); i++) r[i] = (wchar_t)towlower(r[i]);
        return r;
    }

    bool ParseKeyPath(const std::wstring& text0, KeyPath& out)
    {
        std::wstring t = text0;
        for (size_t i = 0; i < t.size(); i++) if (t[i] == L'/') t[i] = L'\\';
        while (!t.empty() && (iswspace(t[0]) || t[0] == L'\\')) t.erase(0, 1);
        while (!t.empty() && (iswspace(t[t.size() - 1]) || t[t.size() - 1] == L'\\')) t.erase(t.size() - 1);
        out = KeyPath();
        if (LowerCopy(t.substr(0, 9)) == L"computer\\") t = t.substr(9);
        if (LowerCopy(t) == L"computer" || t.empty()) return true;
        size_t p = t.find(L'\\');
        std::wstring head = LowerCopy(p == std::wstring::npos ? t : t.substr(0, p));
        std::wstring rest = p == std::wstring::npos ? L"" : t.substr(p + 1);
        for (int i = 0; i < 5; i++)
        {
            if (head == LowerCopy(HiveShort[i]) || head == LowerCopy(HiveNames[i]))
            {
                out.hive = i;
                while (!rest.empty() && rest[0] == L'\\') rest.erase(0, 1);
                out.sub = rest;
                return true;
            }
        }
        return false;
    }

    std::wstring Format(const KeyPath& p)
    {
        if (p.hive < 0) return L"";
        return p.sub.empty() ? std::wstring(HiveShort[p.hive]) : std::wstring(HiveShort[p.hive]) + L"\\" + p.sub;
    }

    KeyPath Child(const KeyPath& p, const std::wstring& name)
    {
        KeyPath c = p; c.sub = p.sub.empty() ? name : p.sub + L"\\" + name; return c;
    }

    KeyPath Parent(const KeyPath& p)
    {
        KeyPath c = p;
        if (p.hive < 0) return c;
        size_t s = p.sub.find_last_of(L'\\');
        if (s != std::wstring::npos) c.sub = p.sub.substr(0, s);
        else if (!p.sub.empty()) c.sub.clear();
        else c.hive = -1;
        return c;
    }

    std::wstring TypeName(uint32_t t)
    {
        switch (t)
        {
        case None: return L"REG_NONE"; case Sz: return L"REG_SZ"; case ExpandSz: return L"REG_EXPAND_SZ";
        case Binary: return L"REG_BINARY"; case Dword: return L"REG_DWORD"; case DwordBE: return L"REG_DWORD_BIG_ENDIAN";
        case Link: return L"REG_LINK"; case MultiSz: return L"REG_MULTI_SZ"; case Qword: return L"REG_QWORD";
        }
        std::wstringstream s; s << L"REG_TYPE_" << t; return s.str();
    }

    std::wstring Hex(const std::vector<unsigned char>& d, size_t max)
    {
        static const wchar_t* h = L"0123456789ABCDEF";
        std::wstring r;
        for (size_t i = 0; i < d.size() && i < max; i++) { if (i) r += L' '; r += h[d[i] >> 4]; r += h[d[i] & 15]; }
        if (d.size() > max) r += L" ...";
        return r;
    }

    static std::wstring Units(const std::vector<unsigned char>& d)
    {
        std::wstring r;
        for (size_t i = 0; i + 1 < d.size(); i += 2) r.push_back((wchar_t)(d[i] | (d[i + 1] << 8)));
        return r;
    }

    static uint32_t U32(const std::vector<unsigned char>& d, bool big)
    {
        uint32_t v = big ? ((uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]) : ((uint32_t)d[3] << 24 | d[2] << 16 | d[1] << 8 | d[0]);
        return v;
    }

    std::wstring ValueToText(uint32_t type, const std::vector<unsigned char>& data)
    {
        std::wstringstream s;
        switch (type)
        {
        case Sz: case ExpandSz: case Link:
        {
            std::wstring r = Units(data);
            size_t z = r.find(L'\0'); if (z != std::wstring::npos) r.erase(z);
            return r;
        }
        case MultiSz:
        {
            std::wstring r = Units(data);
            while (!r.empty() && r[r.size() - 1] == 0) r.erase(r.size() - 1);
            for (size_t i = 0; i < r.size(); i++) if (r[i] == 0) r[i] = L'\n';
            return r;
        }
        case Dword: case DwordBE:
            if (data.size() >= 4) { uint32_t d = U32(data, type == DwordBE); s << L"0x" << std::hex << d << L" (" << std::dec << d << L")"; return s.str(); }
            break;
        case Qword:
            if (data.size() >= 8)
            {
                unsigned long long d = 0; for (int i = 7; i >= 0; i--) d = (d << 8) | data[i];
                s << L"0x" << std::hex << d << L" (" << std::dec << d << L")"; return s.str();
            }
            break;
        }
        return Hex(data, 262144);
    }

    std::wstring ValueSummary(uint32_t type, const std::vector<unsigned char>& data)
    {
        std::wstring t = ValueToText(type, data);
        if (type == Binary || (type != Sz && type != ExpandSz && type != MultiSz && type != Link && type != Dword && type != DwordBE && type != Qword))
        {
            std::wstringstream s; s << data.size() << L" byte: " << Hex(data, 16); return s.str();
        }
        for (size_t i = 0; i < t.size(); i++) if (t[i] == L'\n' || t[i] == L'\r') t[i] = L'|';
        if (t.size() > 120) t = t.substr(0, 120) + L"...";
        return t;
    }

    static bool ParseNum(const std::wstring& t0, unsigned long long& out)
    {
        std::wstring t = t0;
        while (!t.empty() && iswspace(t[0])) t.erase(0, 1);
        while (!t.empty() && iswspace(t[t.size() - 1])) t.erase(t.size() - 1);
        if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X'))
        {
            size_t p = t.find(L' '); if (p != std::wstring::npos) t = t.substr(0, p);   // "0x1F (31)" accettato
        }
        if (t.empty()) return false;
        int base = 10;
        if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) { base = 16; t = t.substr(2); }
        unsigned long long v = 0;
        for (size_t i = 0; i < t.size(); i++)
        {
            int d; wchar_t c = t[i];
            if (c >= L'0' && c <= L'9') d = c - L'0'; else if (c >= L'a' && c <= L'f') d = c - L'a' + 10; else if (c >= L'A' && c <= L'F') d = c - L'A' + 10; else return false;
            if (d >= base) return false;
            if (v > (0xFFFFFFFFFFFFFFFFULL - d) / base) return false;
            v = v * base + d;
        }
        out = v; return true;
    }

    static void PutUnits(std::vector<unsigned char>& data, const std::wstring& s)
    {
        for (size_t i = 0; i < s.size(); i++) { unsigned v = (unsigned)s[i] & 0xFFFF; data.push_back((unsigned char)(v & 0xFF)); data.push_back((unsigned char)(v >> 8)); }
    }

    bool TextToValue(uint32_t type, const std::wstring& text, std::vector<unsigned char>& data, std::wstring& error)
    {
        data.clear();
        switch (type)
        {
        case Sz: case ExpandSz: case Link:
            PutUnits(data, text); data.push_back(0); data.push_back(0); return true;
        case MultiSz:
        {
            std::wstring s;
            for (size_t i = 0; i < text.size(); i++) { if (text[i] == L'\r') continue; s.push_back(text[i] == L'\n' ? L'\0' : text[i]); }
            if (!s.empty()) s.push_back(0);
            s.push_back(0);
            PutUnits(data, s); if (data.size() < 4) { data.push_back(0); data.push_back(0); }
            return true;
        }
        case Dword: case DwordBE: case Qword:
        {
            unsigned long long v;
            if (!ParseNum(text, v)) { error = L"Numero non valido (decimale oppure 0x esadecimale)"; return false; }
            if (type == Qword) { for (int i = 0; i < 8; i++) data.push_back((unsigned char)(v >> (8 * i))); return true; }
            if (v > 0xFFFFFFFFULL) { error = L"Valore troppo grande per un DWORD"; return false; }
            for (int i = 0; i < 4; i++) data.push_back((unsigned char)(v >> (type == DwordBE ? 8 * (3 - i) : 8 * i)));
            return true;
        }
        default:
        {
            int hi = -1;
            for (size_t i = 0; i < text.size(); i++)
            {
                wchar_t c = text[i]; int d;
                if (c >= L'0' && c <= L'9') d = c - L'0'; else if (c >= L'a' && c <= L'f') d = c - L'a' + 10; else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
                else if (iswspace(c) || c == L',') { if (hi >= 0) { error = L"Byte esadecimale incompleto"; return false; } continue; }
                else { error = L"Carattere non valido (servono byte esadecimali)"; return false; }
                if (hi < 0) hi = d; else { data.push_back((unsigned char)(hi * 16 + d)); hi = -1; }
            }
            if (hi >= 0) { error = L"Numero dispari di cifre esadecimali"; return false; }
            return true;
        }
        }
    }

    std::wstring FormatSize(uint64_t b)
    {
        std::wstringstream s;
        if (b < 1024) s << b << L" B";
        else if (b < 1024 * 1024) s << (b * 10 / 1024) / 10 << L"." << (b * 10 / 1024) % 10 << L" KB";
        else if (b < 1024ULL * 1024 * 1024) { uint64_t t = b * 10 / (1024 * 1024); s << t / 10 << L"." << t % 10 << L" MB"; }
        else { uint64_t t = b * 10 / (1024ULL * 1024 * 1024); s << t / 10 << L"." << t % 10 << L" GB"; }
        return s.str();
    }
}
