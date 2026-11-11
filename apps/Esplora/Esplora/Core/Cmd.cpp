#include "Cmd.h"
#include "Root.h"
#include "Fs.h"
#include "Proc.h"
#include "Text.h"
#include "RegText.h"
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cwctype>

namespace
{
    using namespace RegText;

    // the phone SDK hides the executable page flags
    const DWORD kExecRead = 0x20, kExecReadWrite = 0x40;
    typedef LPVOID (WINAPI *PVirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
    typedef BOOL (WINAPI *PVirtualProtect)(LPVOID, SIZE_T, DWORD, PDWORD);
    typedef BOOL (WINAPI *PVirtualFree)(LPVOID, SIZE_T, DWORD);
    typedef BOOL (WINAPI *PFlushInstructionCache)(HANDLE, LPCVOID, SIZE_T);
    typedef int (*PSmall)();

    // calls Thumb code at p, -1 on a fault (no C++ objects here: __try)
    static int CallAt(void* p)
    {
        __try { return ((PSmall)((ULONG_PTR)p | 1))(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // writes "movs r0,#42; bx lr" at p and runs it: 42 = executable
    static int CallStub(void* p, PFlushInstructionCache flush)
    {
        unsigned short code[2] = { 0x202A, 0x4770 };
        memcpy(p, code, sizeof(code));
        if (flush) flush(GetCurrentProcess(), p, sizeof(code));
        return CallAt(p);
    }

    // Can this app process make executable memory (what WebKit's JIT needs)?
    std::wstring JitTest()
    {
        std::wstringstream o;
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((const void*)&VirtualQuery, &mbi, sizeof(mbi));
        HMODULE kb = (HMODULE)mbi.AllocationBase;
        PVirtualAlloc alloc = (PVirtualAlloc)GetProcAddress(kb, "VirtualAlloc");
        PVirtualProtect protect = (PVirtualProtect)GetProcAddress(kb, "VirtualProtect");
        PVirtualFree release = (PVirtualFree)GetProcAddress(kb, "VirtualFree");
        PFlushInstructionCache flush = (PFlushInstructionCache)GetProcAddress(kb, "FlushInstructionCache");
        o << L"VirtualAlloc " << (alloc ? L"ok" : L"NO") << L", VirtualProtect " << (protect ? L"ok" : L"NO") << L"\n";
        if (!alloc || !protect) return o.str();

        void* rwx = alloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, kExecReadWrite);
        o << L"RWX alloc: " << (rwx ? L"ok" : L"ERRORE " + Fs::ErrorText(GetLastError())) << L"\n";
        if (rwx) { o << L"RWX esegue: " << CallStub(rwx, flush) << L"\n"; release(rwx, 0, MEM_RELEASE); }

        void* rw = alloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        o << L"RW alloc: " << (rw ? L"ok" : L"ERRORE " + Fs::ErrorText(GetLastError())) << L"\n";
        if (rw)
        {
            unsigned short code[2] = { 0x202A, 0x4770 };
            memcpy(rw, code, sizeof(code));
            DWORD old = 0;
            BOOL ok = protect(rw, 4096, kExecRead, &old);
            o << L"RW->RX protect: " << (ok ? L"ok" : L"ERRORE " + Fs::ErrorText(GetLastError())) << L"\n";
            if (ok)
            {
                if (flush) flush(GetCurrentProcess(), rw, 4);
                o << L"RX esegue: " << CallAt(rw) << L"\n";
                ok = protect(rw, 4096, kExecReadWrite, &old);
                o << L"RX->RWX protect: " << (ok ? L"ok" : L"ERRORE " + Fs::ErrorText(GetLastError())) << L"\n";
            }
            release(rw, 0, MEM_RELEASE);
        }
        // WebKit reserves its whole JIT pool at once (16-64 MB on 32-bit)
        const int sizes[] = { 1, 8, 16, 32, 64, 128 };
        for (int i = 0; i < 6; i++)
        {
            for (int mode = 0; mode < 3; mode++)
            {
                DWORD type = mode == 2 ? (MEM_RESERVE | MEM_COMMIT) : MEM_RESERVE;
                DWORD prot = mode == 0 ? PAGE_NOACCESS : kExecReadWrite;
                void* res = alloc(nullptr, (SIZE_T)sizes[i] << 20, type, prot);
                o << L"  " << sizes[i] << L" MB " << (mode == 0 ? L"reserve NOACCESS" : mode == 1 ? L"reserve RWX" : L"reserve+commit RWX")
                  << L": " << (res ? L"ok" : L"ERRORE " + Fs::ErrorText(GetLastError())) << L"\n";
                if (res && mode == 0)
                {
                    void* c = alloc(res, 65536, MEM_COMMIT, kExecReadWrite);
                    o << L"      commit 64 KB RWX dentro: " << (c ? L"ok" : L"ERRORE " + Fs::ErrorText(GetLastError())) << L"\n";
                }
                if (res) release(res, 0, MEM_RELEASE);
            }
        }
        return o.str();
    }
    const size_t kMaxOut = 400000;

    struct Out
    {
        std::wstringstream s;
        bool Full() { return (size_t)s.tellp() > kMaxOut; }
    };

    std::wstring Err(LONG r) { return Fs::ErrorText((DWORD)r); }

    bool OpenKey(const KeyPath& p, HKEY& k, LONG& r)
    {
        if (p.hive < 0) { r = 0; return false; }
        r = Root::RegOpen(Root::Hive(p.hive), p.sub, KEY_READ, k);
        return r == 0;
    }

    void ShowKey(const KeyPath& p, Out& o)
    {
        if (p.hive < 0) { for (int i = 0; i < 5; i++) o.s << L"[" << HiveShort[i] << L"]\n"; return; }
        HKEY k; LONG r;
        if (!OpenKey(p, k, r)) { o.s << L"errore apertura: " << Err(r) << L"\n"; return; }
        std::vector<std::wstring> subs; std::vector<Value> vals;
        Root::RegEnumSubKeys(k, subs); LONG rv = Root::RegEnumValues(k, vals);
        for (size_t i = 0; i < subs.size() && !o.Full(); i++) o.s << L"[" << subs[i] << L"]\n";
        for (size_t i = 0; i < vals.size() && !o.Full(); i++)
            o.s << (vals[i].name.empty() ? L"(predefinito)" : vals[i].name) << L" : " << TypeName(vals[i].type) << L" = " << ValueToText(vals[i].type, vals[i].data) << L"\n";
        if (rv) o.s << L"(valori: " << Err(rv) << L")\n";
        Root::RegClose(k);
    }

    void Tree(const KeyPath& p, int depth, Out& o, int indent)
    {
        HKEY k; LONG r;
        if (!OpenKey(p, k, r)) { o.s << std::wstring(indent, L' ') << L"!" << Err(r) << L"\n"; return; }
        std::vector<std::wstring> subs; std::vector<Value> vals;
        Root::RegEnumSubKeys(k, subs); Root::RegEnumValues(k, vals); Root::RegClose(k);
        for (size_t i = 0; i < vals.size() && !o.Full(); i++)
            o.s << std::wstring(indent, L' ') << L"- " << (vals[i].name.empty() ? L"(predefinito)" : vals[i].name) << L" : " << TypeName(vals[i].type) << L" = " << ValueSummary(vals[i].type, vals[i].data) << L"\n";
        for (size_t i = 0; i < subs.size() && !o.Full(); i++)
        {
            o.s << std::wstring(indent, L' ') << L"[" << subs[i] << L"]\n";
            if (depth > 1) Tree(Child(p, subs[i]), depth - 1, o, indent + 2);
        }
    }

    void FindFiles(const std::wstring& dir, const std::wstring& needle, int depth, Out& o, int& found, ULONGLONG deadline)
    {
        if (o.Full() || found > 400 || GetTickCount64() > deadline) return;
        std::vector<Fs::Entry> e;
        if (!Fs::List(dir, e).empty()) return;
        for (size_t i = 0; i < e.size(); i++)
        {
            std::wstring full = dir + (dir[dir.size() - 1] == L'\\' ? L"" : L"\\") + e[i].name;
            if (LowerCopy(e[i].name).find(needle) != std::wstring::npos) { o.s << full << (e[i].isDir ? L"\\" : L"") << L"\n"; found++; }
            if (e[i].isDir && depth > 1 && !(e[i].attributes & FILE_ATTRIBUTE_REPARSE_POINT)) FindFiles(full, needle, depth - 1, o, found, deadline);
        }
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && iswspace(s[a])) a++;
        while (b > a && iswspace(s[b - 1])) b--;
        return s.substr(a, b - a);
    }
}

namespace Cmd
{
    static void SearchRec(const RegText::KeyPath& p, const std::wstring& needle, int depth, size_t maxHits, ULONGLONG deadline, std::vector<Hit>& hits)
    {
        using namespace RegText;
        if (hits.size() >= maxHits || GetTickCount64() > deadline) return;
        HKEY k; LONG r;
        if (p.hive < 0) return;
        r = Root::RegOpen(Root::Hive(p.hive), p.sub, KEY_READ, k);
        if (r != 0) return;
        std::vector<std::wstring> subs; std::vector<Value> vals;
        Root::RegEnumSubKeys(k, subs); Root::RegEnumValues(k, vals); Root::RegClose(k);
        for (size_t i = 0; i < vals.size() && hits.size() < maxHits; i++)
        {
            bool isStr = vals[i].type == Sz || vals[i].type == ExpandSz || vals[i].type == MultiSz || vals[i].type == Link;
            std::wstring text = isStr ? ValueToText(vals[i].type, vals[i].data) : L"";
            if (LowerCopy(vals[i].name).find(needle) != std::wstring::npos || LowerCopy(text).find(needle) != std::wstring::npos)
            {
                Hit h; h.key = Format(p); h.valueName = vals[i].name.empty() ? L"(predefinito)" : vals[i].name; h.isKey = false;
                h.summary = TypeName(vals[i].type) + L" = " + ValueSummary(vals[i].type, vals[i].data);
                hits.push_back(h);
            }
        }
        for (size_t i = 0; i < subs.size() && hits.size() < maxHits; i++)
        {
            if (LowerCopy(subs[i]).find(needle) != std::wstring::npos) { Hit h; h.key = Format(Child(p, subs[i])); h.isKey = true; hits.push_back(h); }
            if (depth > 1) SearchRec(Child(p, subs[i]), needle, depth - 1, maxHits, deadline, hits);
        }
    }

    void SearchRegistry(const RegText::KeyPath& p, const std::wstring& needleLower, int depth, size_t maxHits, unsigned timeoutMs, std::vector<Hit>& hits)
    {
        SearchRec(p, needleLower, depth, maxHits, GetTickCount64() + timeoutMs, hits);
    }

    std::wstring SplitFirst(std::wstring& s)
    {
        s = Trim(s);
        size_t p = s.find(L' ');
        std::wstring h = p == std::wstring::npos ? s : s.substr(0, p);
        s = p == std::wstring::npos ? L"" : Trim(s.substr(p + 1));
        return h;
    }

    // l'ultimo argomento numerico opzionale viene staccato
    static int TakeNumber(std::wstring& rest, int def)
    {
        size_t p = rest.find_last_of(L' ');
        std::wstring last = p == std::wstring::npos ? rest : rest.substr(p + 1);
        if (last.empty() || last.find_first_not_of(L"0123456789") != std::wstring::npos) return def;
        if (p == std::wstring::npos) return def;   // un solo argomento: e' il percorso
        rest = Trim(rest.substr(0, p));
        return _wtoi(last.c_str());
    }

    std::wstring Run(const std::wstring& line0)
    {
        Out o;
        std::wstring rest = line0;
        std::wstring cmd = SplitFirst(rest);
        std::wstring err;
        if (!Root::Init(err)) { return L"Root::Init fallito: " + err; }
        ULONGLONG deadline = GetTickCount64() + 90000;
        if (cmd == L"ls")
        {
            std::vector<Fs::Entry> e; std::wstring r = Fs::List(rest, e);
            if (!r.empty()) return L"errore: " + r;
            for (size_t i = 0; i < e.size() && !o.Full(); i++)
                o.s << (e[i].isDir ? L"<DIR> " : L"      ") << e[i].name << L"  " << (e[i].isDir ? L"" : FormatSize(e[i].size)) << L"  attr=" << std::hex << e[i].attributes << std::dec << L"\n";
        }
        else if (cmd == L"cat")
        {
            int max = TakeNumber(rest, 20000);
            std::vector<unsigned char> b; std::wstring r = Fs::ReadFile(rest, b, 4 * 1024 * 1024);
            if (!r.empty()) return L"errore: " + r;
            TextCodec::Document d; std::wstring e;
            if (TextCodec::Decode(b, d, e)) { if ((int)d.text.size() > max) d.text.resize(max); o.s << d.text; }
            else { o.s << L"(binario " << b.size() << L" byte) " << Hex(std::vector<unsigned char>(b.begin(), b.begin() + std::min<size_t>(b.size(), (size_t)max)), (size_t)max); }
        }
        else if (cmd == L"find")
        {
            int depth = TakeNumber(rest, 4); std::wstring dir = SplitFirst(rest); int found = 0;
            FindFiles(dir, LowerCopy(rest), depth, o, found, deadline);
        }
        else if (cmd == L"reg" || cmd == L"regtree" || cmd == L"regfind")
        {
            int depth = cmd == L"regtree" ? TakeNumber(rest, 2) : (cmd == L"regfind" ? TakeNumber(rest, 6) : 1);
            std::wstring keytext = rest, needle;
            if (cmd == L"regfind")
            {
                // formato: regfind <chiave senza spazi> <testo> [profondita']
                keytext = SplitFirst(rest); needle = LowerCopy(rest);
            }
            KeyPath p;
            if (!ParseKeyPath(keytext, p)) return L"percorso non valido";
            if (cmd == L"reg") ShowKey(p, o);
            else if (cmd == L"regtree") Tree(p, depth, o, 0);
            else { std::vector<Hit> hits; SearchRegistry(p, needle, depth, 300, 90000, hits); for (size_t i = 0; i < hits.size(); i++) o.s << (hits[i].isKey ? L"KEY " : L"") << hits[i].key << (hits[i].isKey ? L"" : L" : " + hits[i].valueName + L" = " + hits[i].summary) << L"\n"; o.s << L"-- trovati " << hits.size() << L"\n"; }
        }
        else if (cmd == L"cp" || cmd == L"mv")
        {
            // cp|mv <sorgente> <destinazione>  (percorsi senza spazi)
            std::wstring src = SplitFirst(rest);
            std::wstring r = cmd == L"cp" ? Fs::Copy(src, rest) : Fs::Move(src, rest);
            o.s << (r.empty() ? L"OK" : L"ERRORE " + r) << L"\n";
        }
        else if (cmd == L"rm")
        {
            std::wstring r = Fs::Delete(rest, false);
            o.s << (r.empty() ? L"OK" : L"ERRORE " + r) << L"\n";
        }
        else if (cmd == L"regset" || cmd == L"regdel")
        {
            // regset <chiave senza spazi> <nome|@> <SZ|EXPAND_SZ|DWORD|MULTI_SZ|BINARY> <dati>  (MULTI_SZ: righe separate da |, BINARY: byte esadecimali)   /   regdel <chiave> <nome|@>
            // nella chiave gli spazi si scrivono %20 (es. Internet%20Explorer)
            // prima stampa il valore attuale (copia di sicurezza nel log sul PC), poi scrive
            KeyPath p;
            std::wstring keytext = SplitFirst(rest);
            for (size_t i; (i = keytext.find(L"%20")) != std::wstring::npos; ) keytext.replace(i, 3, L" ");
            if (!ParseKeyPath(keytext, p) || p.hive < 0) return L"percorso non valido";
            std::wstring name = SplitFirst(rest); if (name == L"@") name.clear();
            HKEY k; LONG r = Root::RegCreate(Root::Hive(p.hive), p.sub, k);
            if (r) return L"errore apertura: " + Err(r);
            std::vector<Value> vals; Root::RegEnumValues(k, vals);
            bool had = false;
            for (size_t i = 0; i < vals.size(); i++)
                if (LowerCopy(vals[i].name) == LowerCopy(name)) { had = true; o.s << L"PRIMA " << TypeName(vals[i].type) << L" = " << ValueToText(vals[i].type, vals[i].data) << L"\n"; }
            if (!had) o.s << L"PRIMA (assente)\n";
            if (cmd == L"regdel") r = had ? Root::RegDelValue(k, name) : 0;
            else
            {
                std::wstring t = SplitFirst(rest);
                uint32_t type = t == L"DWORD" ? Dword : (t == L"EXPAND_SZ" ? ExpandSz : (t == L"SZ" ? Sz :
                                (t == L"MULTI_SZ" ? MultiSz : (t == L"BINARY" ? Binary : None))));
                if (type == MultiSz) std::replace(rest.begin(), rest.end(), L'|', L'\n');
                std::vector<unsigned char> data; std::wstring e;
                if (type == None) { Root::RegClose(k); return o.s.str() + L"tipo non valido (SZ, EXPAND_SZ, DWORD, MULTI_SZ, BINARY)"; }
                if (!TextToValue(type, rest, data, e)) { Root::RegClose(k); return o.s.str() + L"dati non validi: " + e; }
                r = Root::RegSet(k, name, type, data.data(), (DWORD)data.size());
            }
            Root::RegClose(k);
            o.s << (r ? L"ERRORE " + Err(r) : std::wstring(L"OK")) << L"\n";
        }
        else if (cmd == L"run")
        {
            // run [-w <ms>] <exe> [argomenti]
            unsigned long wait = 0;
            if (rest.compare(0, 3, L"-w ") == 0)
            {
                rest = rest.substr(3);
                wait = (unsigned long)_wtoi(SplitFirst(rest).c_str());
            }
            o.s << Proc::Run(rest, wait) << L"\n";
        }
        else if (cmd == L"launch")
        {
            o.s << Proc::Launch(rest) << L"\n";
        }
        else if (cmd == L"svc")
        {
            o.s << Proc::Service(rest) << L"\n";
        }
        else if (cmd == L"jit")
        {
            o.s << JitTest();
        }
        else return L"comandi: ls, cat, find, reg, regtree, regfind, regset, regdel, cp, mv, rm, run, svc, launch, jit (put/get/open/shot dal PC)";
        return o.s.str();
    }
}
