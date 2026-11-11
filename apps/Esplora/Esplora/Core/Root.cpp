#include "Root.h"
#include "RegText.h"
#include <sstream>
#include <algorithm>

namespace Root
{
    typedef LONG (WINAPI *PRegOpenKeyExW)(HKEY, LPCWSTR, DWORD, DWORD, PHKEY);
    typedef LONG (WINAPI *PRegCloseKey)(HKEY);
    typedef LONG (WINAPI *PRegEnumKeyExW)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
    typedef LONG (WINAPI *PRegEnumValueW)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    typedef LONG (WINAPI *PRegQueryInfoKeyW)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
    typedef LONG (WINAPI *PRegSetValueExW)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
    typedef LONG (WINAPI *PRegCreateKeyExW)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
    typedef LONG (WINAPI *PRegDeleteValueW)(HKEY, LPCWSTR);
    typedef LONG (WINAPI *PRegDeleteTreeW)(HKEY, LPCWSTR);
    typedef LONG (WINAPI *PRegDeleteKeyW)(HKEY, LPCWSTR);

    static PRegOpenKeyExW pOpen; static PRegCloseKey pClose; static PRegEnumKeyExW pEnumKey; static PRegEnumValueW pEnumVal;
    static PRegQueryInfoKeyW pInfo; static PRegSetValueExW pSet; static PRegCreateKeyExW pCreate; static PRegDeleteValueW pDelVal;
    static PRegDeleteTreeW pDelTree; static PRegDeleteKeyW pDelKey;
    static std::wstring g_log;
    static bool g_init = false, g_ok = false;

    static HMODULE ModuleOf(const void* addr)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return nullptr;
        return (HMODULE)mbi.AllocationBase;
    }

    template <typename T> static bool Get(HMODULE m, const char* name, T& fn)
    {
        fn = (T)GetProcAddress(m, name);
        std::wstringstream s; s << name << (fn ? L" ok; " : L" NO; ");
        g_log += s.str();
        return fn != nullptr;
    }

    bool Init(std::wstring& error)
    {
        if (g_init) { if (!g_ok) error = g_log; return g_ok; }
        g_init = true;
        // kernelbase.dll: il modulo che contiene VirtualQuery (niente GetModuleHandle)
        HMODULE kb = ModuleOf((const void*)&VirtualQuery);
        std::wstringstream s; s << L"kernelbase base=" << std::hex << (ULONG_PTR)kb << L"; ";
        g_log += s.str();
        if (!kb) { error = g_log; return false; }
        bool a = Get(kb, "RegOpenKeyExW", pOpen);
        Get(kb, "RegCloseKey", pClose);
        Get(kb, "RegEnumKeyExW", pEnumKey);
        Get(kb, "RegEnumValueW", pEnumVal);
        Get(kb, "RegQueryInfoKeyW", pInfo);
        Get(kb, "RegSetValueExW", pSet);
        Get(kb, "RegCreateKeyExW", pCreate);
        Get(kb, "RegDeleteValueW", pDelVal);
        Get(kb, "RegDeleteTreeW", pDelTree);
        Get(kb, "RegDeleteKeyW", pDelKey);
        g_ok = a && pClose && pEnumKey && pEnumVal && pInfo && pSet && pCreate && pDelVal;
        if (!g_ok) error = g_log;
        return g_ok;
    }

    std::wstring InitLog() { return g_log; }

    LONG RegOpen(HKEY root, const std::wstring& subkey, DWORD access, HKEY& out)
    {
        std::wstring e; if (!Init(e)) return ERROR_PROC_NOT_FOUND;
        out = nullptr;
        return pOpen(root, subkey.c_str(), 0, access, &out);
    }
    LONG RegClose(HKEY k) { return pClose ? pClose(k) : ERROR_PROC_NOT_FOUND; }

    LONG RegEnumSubKeys(HKEY k, std::vector<std::wstring>& names)
    {
        names.clear();
        DWORD subs = 0, maxLen = 0;
        LONG r = pInfo(k, nullptr, nullptr, nullptr, &subs, &maxLen, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (r != 0) return r;
        std::vector<wchar_t> buf(std::max<DWORD>(maxLen, 255) + 2);
        for (DWORD i = 0; ; i++)
        {
            DWORD n = (DWORD)buf.size();
            r = pEnumKey(k, i, buf.data(), &n, nullptr, nullptr, nullptr, nullptr);
            if (r == ERROR_NO_MORE_ITEMS) return 0;
            if (r == ERROR_MORE_DATA) { buf.resize(buf.size() * 2); i--; continue; }
            if (r != 0) return r;
            names.push_back(std::wstring(buf.data(), n));
        }
    }

    LONG RegEnumValues(HKEY k, std::vector<RegText::Value>& values)
    {
        values.clear();
        DWORD nvals = 0, maxName = 0, maxData = 0;
        LONG r = pInfo(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &nvals, &maxName, &maxData, nullptr, nullptr);
        if (r != 0) return r;
        std::vector<wchar_t> nb(std::max<DWORD>(maxName, 255) + 2);
        std::vector<unsigned char> db(std::max<DWORD>(maxData, 16) + 2);
        for (DWORD i = 0; ; i++)
        {
            DWORD nn = (DWORD)nb.size(), dn = (DWORD)db.size(), t = 0;
            r = pEnumVal(k, i, nb.data(), &nn, nullptr, &t, db.data(), &dn);
            if (r == ERROR_NO_MORE_ITEMS) return 0;
            if (r == ERROR_MORE_DATA) { nb.resize(nb.size() * 2); db.resize(db.size() * 2); i--; continue; }
            if (r != 0) return r;
            RegText::Value v; v.name.assign(nb.data(), nn); v.type = t; v.data.assign(db.begin(), db.begin() + dn);
            values.push_back(v);
        }
    }

    LONG RegSet(HKEY k, const std::wstring& name, DWORD type, const unsigned char* data, DWORD size)
    {
        std::wstring e; if (!Init(e)) return ERROR_PROC_NOT_FOUND;
        static const unsigned char z = 0;
        return pSet(k, name.c_str(), 0, type, size ? data : &z, size);
    }
    LONG RegCreate(HKEY root, const std::wstring& subkey, HKEY& out)
    {
        std::wstring e; if (!Init(e)) return ERROR_PROC_NOT_FOUND;
        out = nullptr; DWORD disp = 0;
        return pCreate(root, subkey.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &out, &disp);
    }
    LONG RegDelValue(HKEY k, const std::wstring& name) { return pDelVal ? pDelVal(k, name.c_str()) : ERROR_PROC_NOT_FOUND; }
    LONG RegDelKeyTree(HKEY root, const std::wstring& subkey)
    {
        if (pDelTree) return pDelTree(root, subkey.c_str());
        // ripiego ricorsivo con RegDeleteKeyW
        if (!pDelKey) return ERROR_PROC_NOT_FOUND;
        HKEY k; LONG r = RegOpen(root, subkey, KEY_READ, k);
        if (r) return r;
        std::vector<std::wstring> subs; RegEnumSubKeys(k, subs); RegClose(k);
        for (size_t i = 0; i < subs.size(); i++)
        {
            r = RegDelKeyTree(root, subkey + L"\\" + subs[i]);
            if (r) return r;
        }
        return pDelKey(root, subkey.c_str());
    }
}
