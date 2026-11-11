#pragma once
// Accesso "root" alle API Win32 fuori dalla partizione app (registro) tramite GetProcAddress su kernelbase.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <stdint.h>
#include "RegText.h"


namespace Root
{
    // Radici del registro (winreg.h non e' nella partizione app)
    inline HKEY Hive(int i)
    {
        static const unsigned long v[] = { 0x80000000, 0x80000001, 0x80000002, 0x80000003, 0x80000005 };
        return (HKEY)(ULONG_PTR)(LONG)v[i];
    }

    bool Init(std::wstring& error);          // risolve le funzioni; false se mancano
    std::wstring InitLog();                  // dettagli di cosa e' stato risolto

    // Tutte le funzioni restituiscono il codice Win32 (0 = ok).
    LONG RegOpen(HKEY root, const std::wstring& subkey, DWORD access, HKEY& out);
    LONG RegClose(HKEY k);
    LONG RegEnumSubKeys(HKEY k, std::vector<std::wstring>& names);
    LONG RegEnumValues(HKEY k, std::vector<RegText::Value>& values);
    LONG RegSet(HKEY k, const std::wstring& name, DWORD type, const unsigned char* data, DWORD size);
    LONG RegCreate(HKEY root, const std::wstring& subkey, HKEY& out);
    LONG RegDelValue(HKEY k, const std::wstring& name);
    LONG RegDelKeyTree(HKEY root, const std::wstring& subkey);
}
