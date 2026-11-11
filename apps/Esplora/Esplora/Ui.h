#pragma once
#include "Core/RegText.h"
#include <memory>
#include <string>

namespace Esplora
{
    std::wstring W(Platform::String^ s);
    Platform::String^ S(const std::wstring& s);

    // true se la scrittura e' in una zona "sicura" (dati utente / chiavi di prova): niente conferma.
    bool IsSafeFilePath(const std::wstring& path);
    bool IsSafeRegPath(const RegText::KeyPath& p);

    concurrency::task<bool> Confirm(Platform::String^ title, Platform::String^ message, Platform::String^ yesText);
    // risultato: true se l'utente ha premuto OK; il testo e' in *out
    concurrency::task<bool> Prompt(Platform::String^ title, const std::wstring& initial, std::shared_ptr<std::wstring> out, bool multiline = false);
    void Info(Platform::String^ title, Platform::String^ message);
    std::wstring FormatDate(const FILETIME& t);
    void GoBackFrame();
}
