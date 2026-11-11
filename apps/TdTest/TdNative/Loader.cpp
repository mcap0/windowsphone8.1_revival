#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include "Loader.h"

using namespace TdNative;
using namespace Platform;

namespace
{
    String^ Err(const wchar_t* what, DWORD code)
    {
        std::wstringstream s;
        s << what << L" " << code;
        switch (code)
        {
        case 126: s << L" (manca una DLL da cui dipende)"; break;
        case 127: s << L" (manca una funzione importata)"; break;
        case 193: s << L" (formato non valido)"; break;
        case 5: s << L" (accesso negato)"; break;
        case 2: s << L" (file non trovato)"; break;
        }
        return ref new String(s.str().c_str());
    }

    bool ReadPackaged(const std::wstring& name, std::vector<unsigned char>& bytes)
    {
        std::wstring path = std::wstring(Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data()) + L"\\" + name;
        HANDLE f = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        FILE_STANDARD_INFO info;
        if (!GetFileInformationByHandleEx(f, FileStandardInfo, &info, sizeof(info))) { CloseHandle(f); return false; }
        bytes.resize(static_cast<size_t>(info.EndOfFile.QuadPart));
        DWORD read = 0;
        BOOL ok = ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(f);
        return ok && read == bytes.size();
    }

    template <typename T> const T* At(const std::vector<unsigned char>& b, size_t off)
    {
        return off + sizeof(T) <= b.size() ? reinterpret_cast<const T*>(&b[off]) : nullptr;
    }

    // file offset of an RVA, through the section table
    size_t Offset(const std::vector<unsigned char>& b, const IMAGE_NT_HEADERS32* nt, size_t ntOff, DWORD rva)
    {
        size_t secOff = ntOff + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            const IMAGE_SECTION_HEADER* s = At<IMAGE_SECTION_HEADER>(b, secOff + i * sizeof(IMAGE_SECTION_HEADER));
            if (!s) break;
            DWORD size = s->Misc.VirtualSize > s->SizeOfRawData ? s->Misc.VirtualSize : s->SizeOfRawData;
            if (rva >= s->VirtualAddress && rva < s->VirtualAddress + size)
                return rva - s->VirtualAddress + s->PointerToRawData;
        }
        return static_cast<size_t>(-1);
    }

    std::string Str(const std::vector<unsigned char>& b, size_t off)
    {
        std::string r;
        while (off < b.size() && b[off] != 0 && r.size() < 260) r += static_cast<char>(b[off++]);
        return r;
    }
}

String^ Loader::TryLoad(String^ name)
{
    HMODULE h = LoadPackagedLibrary(name->Data(), 0);
    if (h != nullptr) return L"ok";
    return Err(L"ERRORE", GetLastError());
}

String^ Loader::CheckImports(String^ name)
{
    std::vector<unsigned char> b;
    if (!ReadPackaged(name->Data(), b)) return Err(L"non riesco a leggere il file, errore", GetLastError());
    const IMAGE_DOS_HEADER* dos = At<IMAGE_DOS_HEADER>(b, 0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return L"non \u00e8 un PE";
    size_t ntOff = static_cast<size_t>(dos->e_lfanew);
    const IMAGE_NT_HEADERS32* nt = At<IMAGE_NT_HEADERS32>(b, ntOff);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return L"PE non a 32 bit";

    std::wstringstream out;
    out << L"subsystem " << nt->OptionalHeader.MajorSubsystemVersion << L"." << nt->OptionalHeader.MinorSubsystemVersion << L"\n";
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t descOff = Offset(b, nt, ntOff, dir.VirtualAddress);
    if (dir.VirtualAddress == 0 || descOff == static_cast<size_t>(-1)) return ref new String((out.str() + L"nessuna import").c_str());

    for (;; descOff += sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        const IMAGE_IMPORT_DESCRIPTOR* d = At<IMAGE_IMPORT_DESCRIPTOR>(b, descOff);
        if (!d || d->Name == 0) break;
        std::string dll = Str(b, Offset(b, nt, ntOff, d->Name));
        std::wstring wdll(dll.begin(), dll.end());
        HMODULE h = LoadPackagedLibrary(wdll.c_str(), 0);
        if (h == nullptr)
        {
            out << wdll << L": NON CARICA (" << GetLastError() << L")\n";
            continue;
        }
        DWORD thunkRva = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        size_t thunkOff = Offset(b, nt, ntOff, thunkRva);
        int missing = 0, total = 0;
        std::wstring names;
        for (; thunkOff != static_cast<size_t>(-1); thunkOff += 4)
        {
            const DWORD* t = At<DWORD>(b, thunkOff);
            if (!t || *t == 0) break;
            total++;
            FARPROC p;
            std::string fn;
            if (*t & IMAGE_ORDINAL_FLAG32)
            {
                p = GetProcAddress(h, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(*t & 0xFFFF)));
                fn = "#" + std::to_string(*t & 0xFFFF);
            }
            else
            {
                fn = Str(b, Offset(b, nt, ntOff, *t) + 2);
                p = GetProcAddress(h, fn.c_str());
            }
            if (p == nullptr)
            {
                if (missing < 12) names += L" " + std::wstring(fn.begin(), fn.end());
                missing++;
            }
        }
        if (missing == 0) out << wdll << L": ok (" << total << L")\n";
        else out << wdll << L": MANCANO " << missing << L"/" << total << L":" << names << (missing > 12 ? L" \u2026" : L"") << L"\n";
    }
    return ref new String(out.str().c_str());
}

String^ Loader::TdVersion()
{
    HMODULE h = LoadPackagedLibrary(L"tdjson.dll", 0);
    if (h == nullptr) return Err(L"tdjson non carica:", GetLastError());
    typedef const char* (*Execute)(const char*);
    Execute exec = reinterpret_cast<Execute>(GetProcAddress(h, "td_execute"));
    if (exec == nullptr) return L"td_execute non trovata";
    const char* r = exec("{\"@type\":\"getOption\",\"name\":\"version\"}");
    std::string s = r ? r : "(null)";
    return ref new String(std::wstring(s.begin(), s.end()).c_str());
}
