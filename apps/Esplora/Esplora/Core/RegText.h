#pragma once
// Logica pura (senza Windows) per il registro: percorsi, tipi, conversione valore <-> testo.
#include <string>
#include <vector>
#include <stdint.h>

namespace RegText
{
    enum Type { None = 0, Sz = 1, ExpandSz = 2, Binary = 3, Dword = 4, DwordBE = 5, Link = 6, MultiSz = 7, Qword = 11 };

    struct Value
    {
        std::wstring name;
        uint32_t type;
        std::vector<unsigned char> data;
        Value() : type(0) {}
    };

    // 0 HKCR, 1 HKCU, 2 HKLM, 3 HKU, 4 HKCC
    extern const wchar_t* const HiveNames[5];
    extern const wchar_t* const HiveShort[5];

    struct KeyPath
    {
        int hive;               // -1 = radice (elenco dei rami)
        std::wstring sub;       // senza backslash iniziale/finale
        KeyPath() : hive(-1) {}
    };

    bool ParseKeyPath(const std::wstring& text, KeyPath& out);   // accetta HKLM\..., HKEY_LOCAL_MACHINE\..., "Computer\..."
    std::wstring Format(const KeyPath& p);                        // forma breve HKLM\sub
    KeyPath Child(const KeyPath& p, const std::wstring& name);
    KeyPath Parent(const KeyPath& p);

    std::wstring TypeName(uint32_t type);
    std::wstring ValueToText(uint32_t type, const std::vector<unsigned char>& data);   // per la modifica
    std::wstring ValueSummary(uint32_t type, const std::vector<unsigned char>& data);  // riga della lista
    bool TextToValue(uint32_t type, const std::wstring& text, std::vector<unsigned char>& data, std::wstring& error);
    std::wstring Hex(const std::vector<unsigned char>& d, size_t max);

    std::wstring FormatSize(uint64_t bytes);
    std::wstring LowerCopy(const std::wstring& s);
}
