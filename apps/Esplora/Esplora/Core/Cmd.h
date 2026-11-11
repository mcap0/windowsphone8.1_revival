#pragma once
#include <string>
#include <vector>
#include "RegText.h"
// Console di diagnostica (SOLO LETTURA): comandi ls/cat/stat/find/reg/regtree/regfind.
namespace Cmd
{
    std::wstring Run(const std::wstring& line);
    struct Hit { std::wstring key; std::wstring valueName; std::wstring summary; bool isKey; Hit() : isKey(false) {} };
    // ricerca (needle gia' minuscolo) nel nome di chiavi/valori e nei dati stringa; usata anche dalla UI
    void SearchRegistry(const RegText::KeyPath& p, const std::wstring& needleLower, int depth, size_t maxHits, unsigned timeoutMs, std::vector<Hit>& hits);
    std::wstring SplitFirst(std::wstring& s);
}
