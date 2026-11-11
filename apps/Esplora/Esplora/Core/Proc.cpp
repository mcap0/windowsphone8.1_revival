#include "Proc.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sstream>

namespace
{
    // STARTUPINFOW / PROCESS_INFORMATION are outside the app partition: same layout, own names
    struct StartInfo
    {
        DWORD cb; LPWSTR reserved, desktop, title;
        DWORD x, y, xSize, ySize, xChars, yChars, fill, flags;
        WORD show, reserved2Size; LPBYTE reserved2;
        HANDLE in, out, err;
    };
    struct ProcInfo { HANDLE process, thread; DWORD processId, threadId; };
    typedef BOOL (WINAPI *PCreateProcessW)(LPCWSTR, LPWSTR, void*, void*, BOOL, DWORD, LPVOID, LPCWSTR, StartInfo*, ProcInfo*);
    typedef BOOL (WINAPI *PGetExitCodeProcess)(HANDLE, LPDWORD);
    typedef HMODULE (WINAPI *PLoadLibraryExW)(LPCWSTR, HANDLE, DWORD);

    // winsvc.h is not in the app partition: the few types and constants used here
    typedef void* SCH;
    struct SvcStatus { DWORD type, state, accepted, exitCode, serviceExitCode, checkPoint, waitHint; };
    typedef SCH (WINAPI *POpenSCManagerW)(LPCWSTR, LPCWSTR, DWORD);
    typedef SCH (WINAPI *POpenServiceW)(SCH, LPCWSTR, DWORD);
    typedef SCH (WINAPI *PCreateServiceW)(SCH, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR,
                                          LPCWSTR, LPCWSTR);
    typedef BOOL (WINAPI *PStartServiceW)(SCH, DWORD, LPCWSTR*);
    typedef BOOL (WINAPI *PControlService)(SCH, DWORD, SvcStatus*);
    typedef BOOL (WINAPI *PQueryServiceStatus)(SCH, SvcStatus*);
    typedef BOOL (WINAPI *PDeleteService)(SCH);
    typedef BOOL (WINAPI *PCloseServiceHandle)(SCH);

    const DWORD ScManagerAll = 0xF003F, ServiceAll = 0xF01FF, OwnProcess = 0x10, AutoStart = 2, DemandStart = 3,
                ErrorIgnore = 0, ControlStop = 1;

    HMODULE KernelBase()
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void*)&VirtualQuery, &mbi, sizeof(mbi)) == 0) return nullptr;
        return (HMODULE)mbi.AllocationBase;
    }

    std::wstring Err(DWORD e)
    {
        std::wstringstream s;
        s << e;
        if (e == 5) s << L" (accesso negato)";
        else if (e == 2) s << L" (file non trovato)";
        else if (e == 1060) s << L" (servizio inesistente)";
        else if (e == 1073) s << L" (servizio gia' esistente)";
        else if (e == 1056) s << L" (gia' in esecuzione)";
        else if (e == 1062) s << L" (non avviato)";
        else if (e == 193) s << L" (eseguibile non valido)";
        return s.str();
    }

    std::wstring First(std::wstring& rest)
    {
        size_t a = rest.find_first_not_of(L' ');
        if (a == std::wstring::npos) { rest.clear(); return std::wstring(); }
        size_t b = rest.find(L' ', a);
        std::wstring w = rest.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        rest = b == std::wstring::npos ? std::wstring() : rest.substr(b + 1);
        return w;
    }

    const wchar_t* StateName(DWORD s)
    {
        switch (s)
        {
        case 1: return L"fermo";
        case 2: return L"in avvio";
        case 3: return L"in arresto";
        case 4: return L"in esecuzione";
        default: return L"?";
        }
    }

    struct Scm
    {
        POpenSCManagerW open; POpenServiceW openService; PCreateServiceW create; PStartServiceW start;
        PControlService control; PQueryServiceStatus query; PDeleteService remove; PCloseServiceHandle close;
        bool Load(std::wstring& error)
        {
            HMODULE kb = KernelBase();
            PLoadLibraryExW load = kb ? (PLoadLibraryExW)GetProcAddress(kb, "LoadLibraryExW") : nullptr;
            HMODULE sec = load ? load(L"sechost.dll", nullptr, 0) : nullptr;
            if (!sec) { error = L"sechost.dll non caricata: " + Err(GetLastError()); return false; }
            open = (POpenSCManagerW)GetProcAddress(sec, "OpenSCManagerW");
            openService = (POpenServiceW)GetProcAddress(sec, "OpenServiceW");
            create = (PCreateServiceW)GetProcAddress(sec, "CreateServiceW");
            start = (PStartServiceW)GetProcAddress(sec, "StartServiceW");
            control = (PControlService)GetProcAddress(sec, "ControlService");
            query = (PQueryServiceStatus)GetProcAddress(sec, "QueryServiceStatus");
            remove = (PDeleteService)GetProcAddress(sec, "DeleteService");
            close = (PCloseServiceHandle)GetProcAddress(sec, "CloseServiceHandle");
            if (!open || !openService || !create || !start || !control || !query || !remove || !close)
            {
                error = L"funzioni SCM mancanti";
                return false;
            }
            return true;
        }
    };
}

std::wstring Proc::Run(const std::wstring& commandLine, unsigned long waitMs)
{
    HMODULE kb = KernelBase();
    PCreateProcessW create = kb ? (PCreateProcessW)GetProcAddress(kb, "CreateProcessW") : nullptr;
    PGetExitCodeProcess exitCode = kb ? (PGetExitCodeProcess)GetProcAddress(kb, "GetExitCodeProcess") : nullptr;
    if (!create) return L"CreateProcessW non trovata";
    std::wstring line = commandLine;
    StartInfo si = {};
    si.cb = sizeof(si);
    ProcInfo pi = {};
    if (!create(nullptr, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return L"ERRORE avvio: " + Err(GetLastError());
    std::wstringstream s;
    s << L"avviato pid " << pi.processId;
    if (waitMs > 0)
    {
        DWORD w = WaitForSingleObjectEx(pi.process, waitMs, FALSE);
        DWORD code = 0;
        if (w == WAIT_OBJECT_0 && exitCode && exitCode(pi.process, &code)) s << L", uscito con " << code << L" (0x" << std::hex << code << L")";
        else s << L", ancora in esecuzione dopo " << waitMs << L" ms";
    }
    CloseHandle(pi.thread);
    CloseHandle(pi.process);
    return s.str();
}

std::wstring Proc::Service(const std::wstring& args)
{
    std::wstring rest = args;
    std::wstring action = First(rest), name = First(rest);
    if (action.empty() || name.empty()) return L"uso: svc query|start|stop|delete <nome>  /  svc create <nome> <exe> [auto|manual]";
    Scm scm;
    std::wstring error;
    if (!scm.Load(error)) return error;
    SCH manager = scm.open(nullptr, nullptr, ScManagerAll);
    if (!manager) return L"ERRORE OpenSCManager: " + Err(GetLastError());
    std::wstringstream s;
    SCH service = nullptr;
    if (action == L"create")
    {
        std::wstring exe = First(rest), mode = First(rest);
        service = scm.create(manager, name.c_str(), name.c_str(), ServiceAll, OwnProcess, mode == L"auto" ? AutoStart : DemandStart,
                             ErrorIgnore, exe.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        s << (service ? L"creato" : L"ERRORE CreateService: " + Err(GetLastError()));
    }
    else
    {
        service = scm.openService(manager, name.c_str(), ServiceAll);
        if (!service)
        {
            s << L"ERRORE OpenService: " << Err(GetLastError());
        }
        else if (action == L"start")
        {
            s << (scm.start(service, 0, nullptr) ? L"avviato" : L"ERRORE StartService: " + Err(GetLastError()));
        }
        else if (action == L"stop")
        {
            SvcStatus st = {};
            s << (scm.control(service, ControlStop, &st) ? L"arresto chiesto" : L"ERRORE ControlService: " + Err(GetLastError()));
        }
        else if (action == L"delete")
        {
            s << (scm.remove(service) ? L"eliminato" : L"ERRORE DeleteService: " + Err(GetLastError()));
        }
        else if (action != L"query")
        {
            s << L"azione sconosciuta";
        }
    }
    if (service)
    {
        SvcStatus st = {};
        if (scm.query(service, &st)) s << L"; stato " << StateName(st.state) << L", uscita " << st.exitCode << L"/" << st.serviceExitCode;
        scm.close(service);
    }
    scm.close(manager);
    return s.str();
}

std::wstring Proc::Launch(const std::wstring& args)
{
    std::wstring rest = args;
    const char* name = "Shell_LaunchSessionByUri";
    bool generic = false;
    if (rest.compare(0, 4, L"gen ") == 0) { name = "?Shell_LaunchSessionGeneric@@YAJPBG00I@Z"; rest = rest.substr(4); generic = true; }
    if (rest.compare(0, 3, L"ex ") == 0) { name = "Shell_LaunchSessionByUriEx"; rest = rest.substr(3); }
    else if (rest.compare(0, 7, L"speech ") == 0) { name = "Shell_LaunchSpeechSessionByUri"; rest = rest.substr(7); }
    HMODULE kb = KernelBase();
    PLoadLibraryExW load = kb ? (PLoadLibraryExW)GetProcAddress(kb, "LoadLibraryExW") : nullptr;
    HMODULE api = load ? load(L"ShellChromeAPI.dll", nullptr, 0) : nullptr;
    if (!api) return L"ShellChromeAPI.dll: errore " + Err(GetLastError());
    typedef HRESULT (WINAPI *PLaunch)(LPCWSTR);
    typedef HRESULT (WINAPI *PGeneric)(LPCWSTR, LPCWSTR, LPCWSTR, unsigned);
    FARPROC fn = GetProcAddress(api, name);
    if (!fn) return std::wstring(L"funzione non trovata");
    HRESULT hr;
    if (generic)
    {
        // gen <uri o AUMID>[|parametri]: come RetailDemoService, (uri, tileId vuoto, parametri, 2)
        size_t bar = rest.find(L'|');
        std::wstring params = bar == std::wstring::npos ? L"" : rest.substr(bar + 1);
        if (bar != std::wstring::npos) rest = rest.substr(0, bar);
        hr = ((PGeneric)fn)(rest.c_str(), L"", params.c_str(), 2);
    }
    else hr = ((PLaunch)fn)(rest.c_str());
    std::wstringstream s;
    s << name << L"(" << rest << L") = 0x" << std::hex << (unsigned)hr;
    return s.str();
}
