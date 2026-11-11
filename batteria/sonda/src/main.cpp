// sonda (ex DumbSvc) — battery probe for the Lumia, running in the InBoxPowerWDSvc boot service slot.
//
// Every minute it records, without touching the network: the battery as the power manager sees it (remaining and
// full-charge capacity, rate: negative = discharging), the driver's percentage, total CPU use and the processes that
// used the most CPU in that minute. The API documents mWh/mW, but the Lumia reports 1900 for a 2000 mAh BV-5XW: the
// numbers are mAh/mA, confirmed against a measured night (see ../PIANO.md), so they are labelled that way.
// Lines go to a file on the phone and are queued; every 5 minutes, if the PC inbox answers, the queue is sent there
// (POST /batteria). Away from home nothing is lost: the queue goes out on return.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cwchar>

extern "C" {
    LONG NTAPI NtPowerInformation(POWER_INFORMATION_LEVEL level, PVOID in, ULONG inLen, PVOID out, ULONG outLen);
    LONG NTAPI NtQuerySystemInformation(ULONG cls, PVOID buf, ULONG len, PULONG ret);
    DWORD WINAPI CM_Get_Device_Interface_List_SizeW(PULONG len, LPGUID cls, PWSTR device, ULONG flags);
    DWORD WINAPI CM_Get_Device_Interface_ListW(LPGUID cls, PWSTR device, PWSTR buffer, ULONG len, ULONG flags);
}

namespace
{
    // The phone runs this executable from the InBoxPowerWDSvc service entry: the dispatcher name must match it.
    const wchar_t* const ServiceName = L"InBoxPowerWDSvc";
    const wchar_t* const LogPath = L"C:\\Data\\Users\\Public\\Documents\\batteria.log";
    const wchar_t* const OldLogPath = L"C:\\Data\\Users\\Public\\Documents\\batteria.old.log";
    const char* const InboxHost = "192.168.1.148";
    const int InboxPort = 8765;
    const DWORD SampleMs = 60 * 1000;
    const ULONGLONG FlushMs = 5 * 60 * 1000;
    const size_t MaxQueue = 1 << 20;        // ~6 days of lines; the file on the phone keeps everything anyway
    const LONGLONG MaxLogBytes = 4 << 20;   // then batteria.log becomes batteria.old.log

    SERVICE_STATUS_HANDLE g_status = nullptr;
    HANDLE g_stop = nullptr;
    bool g_console = false;
    std::string g_queue;

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring Fmt(const wchar_t* f, ...)
    {
        wchar_t b[512];
        va_list a;
        va_start(a, f);
        _vsnwprintf_s(b, _countof(b), _TRUNCATE, f, a);
        va_end(a);
        return b;
    }

    SOCKET Connect(const char* ip, int port, int seconds)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        int r = connect(s, (sockaddr*)&a, sizeof(a));
        if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
        fd_set w, e;
        FD_ZERO(&w); FD_SET(s, &w);
        FD_ZERO(&e); FD_SET(s, &e);
        timeval tv = { seconds, 0 };
        r = select(0, nullptr, &w, &e, &tv);
        if (r <= 0 || FD_ISSET(s, &e)) { closesocket(s); return INVALID_SOCKET; }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        return s;
    }

    bool SendAll(SOCKET s, const std::string& data)
    {
        size_t off = 0;
        while (off < data.size())
        {
            int n = send(s, data.data() + off, (int)std::min<size_t>(data.size() - off, 16384), 0);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }

    // One connection for the whole queue; the queue is emptied only when the inbox answers "200".
    void Flush()
    {
        if (g_queue.empty()) return;
        SOCKET s = Connect(InboxHost, InboxPort, 2);
        if (s == INVALID_SOCKET) return;
        char head[256];
        sprintf_s(head, "POST /batteria HTTP/1.0\r\nHost: %s:%d\r\nContent-Length: %u\r\n\r\n", InboxHost, InboxPort,
                  (unsigned)g_queue.size());
        char buf[64] = {};
        bool ok = SendAll(s, head) && SendAll(s, g_queue) && recv(s, buf, sizeof(buf) - 1, 0) > 0 && strstr(buf, " 200");
        closesocket(s);
        if (ok) g_queue.clear();
    }

    void AppendFile(const std::string& line)
    {
        HANDLE f = CreateFileW(LogPath, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size = {};
        GetFileSizeEx(f, &size);
        DWORD written = 0;
        WriteFile(f, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(f);
        if (size.QuadPart > MaxLogBytes) MoveFileExW(LogPath, OldLogPath, MOVEFILE_REPLACE_EXISTING);
    }

    void Log(const std::wstring& text)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::string line = Utf8(Fmt(L"%02u-%02u %02u:%02u:%02u ", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) + text + L"\r\n");
        AppendFile(line);
        if (g_queue.size() + line.size() > MaxQueue) g_queue.erase(0, g_queue.find('\n', line.size()) + 1);
        g_queue += line;
        if (g_console) fputs(line.c_str(), stdout);
    }

    // ---- battery ----
    //
    // Read straight from the battery driver (battc.sys + qcbattminiclass8960.sys) through the battery class IOCTLs:
    // every IOCTL_BATTERY_QUERY_STATUS reaches the miniclass, so voltage and current are fresh readings. The power
    // manager's SystemBatteryState (the first version of this probe) is a cache refreshed only on wake-ups: the
    // percentage stayed at 100% for an hour off the charger and the rate changed only when the phone woke.

    const GUID BatteryClass = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };
    const DWORD IoctlTag = 0x294040;   // CTL_CODE(FILE_DEVICE_BATTERY, 0x10, METHOD_BUFFERED, FILE_READ_ACCESS)
    const DWORD IoctlInfo = 0x294044;   // function 0x11
    const DWORD IoctlStatus = 0x29404C;   // function 0x13
    const ULONG Unknown = 0xFFFFFFFF;          // BATTERY_UNKNOWN_CAPACITY / _VOLTAGE
    const LONG UnknownRate = (LONG)0x80000000;  // BATTERY_UNKNOWN_RATE

    struct BatteryQuery { ULONG tag; ULONG level; LONG atRate; };   // BATTERY_QUERY_INFORMATION
    struct BatteryInfo                                                  // BATTERY_INFORMATION
    {
        ULONG capabilities;
        UCHAR technology, reserved[3], chemistry[4];
        ULONG designed, full, alert1, alert2, criticalBias, cycles;
    };
    struct BatteryWait { ULONG tag, timeout, powerState, low, high; };  // BATTERY_WAIT_STATUS
    struct BatteryStatus { ULONG powerState, capacity, voltage; LONG rate; };

    HANDLE g_batt = INVALID_HANDLE_VALUE;
    ULONG g_tag = 0;
    BatteryInfo g_info = {};
    ULONGLONG g_infoAt = 0;
    bool g_hasTemp = true;

    std::wstring Cap(ULONG v) { return v == Unknown ? std::wstring(L"?") : Fmt(L"%lu", v); }

    // Finds and opens the first battery, reads its tag and static information; a line describing it goes to the log.
    bool OpenBattery()
    {
        if (g_batt != INVALID_HANDLE_VALUE) { CloseHandle(g_batt); g_batt = INVALID_HANDLE_VALUE; }
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&BatteryClass, nullptr, 0) != 0 || len < 2)
        {
            Log(L"driver batteria: nessuna interfaccia batteria");
            return false;
        }
        std::vector<wchar_t> list(len);
        if (CM_Get_Device_Interface_ListW((LPGUID)&BatteryClass, nullptr, list.data(), len, 0) != 0 || !list[0]) return false;
        g_batt = CreateFileW(list.data(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_batt == INVALID_HANDLE_VALUE)
        {
            Log(Fmt(L"driver batteria: apertura di %s fallita (%lu)", list.data(), GetLastError()));
            return false;
        }
        DWORD wait = 0, got = 0;
        if (!DeviceIoControl(g_batt, IoctlTag, &wait, sizeof(wait), &g_tag, sizeof(g_tag), &got, nullptr) || !g_tag)
        {
            Log(Fmt(L"driver batteria: nessun tag (%lu)", GetLastError()));
            return false;
        }
        BatteryQuery q = { g_tag, 0 /* BatteryInformation */, 0 };
        if (!DeviceIoControl(g_batt, IoctlInfo, &q, sizeof(q), &g_info, sizeof(g_info), &got, nullptr))
        {
            Log(Fmt(L"driver batteria: informazioni non disponibili (%lu)", GetLastError()));
            g_info = {};
        }
        g_infoAt = GetTickCount64();
        wchar_t chem[5] = {};
        for (int i = 0; i < 4; ++i) chem[i] = g_info.chemistry[i] ? (wchar_t)g_info.chemistry[i] : L' ';
        Log(Fmt(L"driver batteria: %s, chimica %s, progetto %s, piena %s, cicli %s, %s", list.data(), chem,
                Cap(g_info.designed).c_str(), Cap(g_info.full).c_str(), Cap(g_info.cycles).c_str(),
                g_info.capabilities & 0x40000000 ? L"capacità relativa (%)" : L"capacità assoluta"));
        return true;
    }

    std::wstring Battery()
    {
        SYSTEM_POWER_STATUS ps = {};
        GetSystemPowerStatus(&ps);
        std::wstring s = Fmt(L"bat %u%%", (unsigned)ps.BatteryLifePercent);

        BatteryWait w = { g_tag, 0, 0, 0, 0 };
        BatteryStatus st = {};
        DWORD got = 0;
        bool ok = g_batt != INVALID_HANDLE_VALUE &&
                  DeviceIoControl(g_batt, IoctlStatus, &w, sizeof(w), &st, sizeof(st), &got, nullptr);
        if (!ok && OpenBattery())   // tag changed or handle lost: reopen once
        {
            w.tag = g_tag;
            ok = DeviceIoControl(g_batt, IoctlStatus, &w, sizeof(w), &st, sizeof(st), &got, nullptr) != FALSE;
        }
        if (!ok)
        {
            // fallback: the power manager's cached state
            SYSTEM_BATTERY_STATE b = {};
            if (NtPowerInformation(SystemBatteryState, nullptr, 0, &b, sizeof(b)) == 0)
                s += Fmt(L" cache %lu/%lu %ld %s", b.RemainingCapacity, b.MaxCapacity, (long)b.Rate,
                         b.AcOnLine ? (b.Charging ? L"carica" : L"rete") : L"batteria");
            return s + Fmt(L" (driver: errore %lu)", GetLastError());
        }

        s += st.voltage == Unknown ? std::wstring(L" ? mV") : Fmt(L" %lu mV", st.voltage);
        s += st.rate == UnknownRate ? std::wstring(L" ? mA") : Fmt(L" %ld mA", (long)st.rate);
        s += L" " + Cap(st.capacity) + L"/" + Cap(g_info.full) + L" mAh";
        s += st.powerState & 4 ? L" carica" : st.powerState & 1 ? L" rete" : L" batteria";
        if (st.powerState & 8) s += L" CRITICA";

        if (g_hasTemp)
        {
            BatteryQuery q = { g_tag, 2 /* BatteryTemperature */, 0 };
            ULONG temp = 0;   // tenths of a kelvin
            if (DeviceIoControl(g_batt, IoctlInfo, &q, sizeof(q), &temp, sizeof(temp), &got, nullptr) && temp && temp != Unknown)
                s += Fmt(L" %.1f C", temp / 10.0 - 273.15);
            else
            {
                g_hasTemp = false;
                s += Fmt(L" (temperatura dal driver non disponibile: %lu)", GetLastError());
            }
        }

        // full-charge capacity and cycles move slowly: refresh every 30 minutes, logged only when they change
        if (GetTickCount64() - g_infoAt > 30 * 60 * 1000)
        {
            BatteryQuery q = { g_tag, 0, 0 };
            BatteryInfo info = {};
            if (DeviceIoControl(g_batt, IoctlInfo, &q, sizeof(q), &info, sizeof(info), &got, nullptr) &&
                (info.full != g_info.full || info.cycles != g_info.cycles))
                s += L" | piena " + Cap(info.full) + L", cicli " + Cap(info.cycles), g_info = info;
            g_infoAt = GetTickCount64();
        }
        return s;
    }

    // ---- CPU ----

    struct ProcInfo   // SYSTEM_PROCESS_INFORMATION, leading fields
    {
        ULONG NextEntryOffset;
        ULONG NumberOfThreads;
        LARGE_INTEGER WorkingSetPrivateSize;
        ULONG HardFaultCount;
        ULONG NumberOfThreadsHighWatermark;
        ULONGLONG CycleTime;
        LARGE_INTEGER CreateTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER KernelTime;
        USHORT NameLength;
        USHORT NameMax;
        PWSTR Name;
        LONG BasePriority;
        HANDLE UniqueProcessId;
    };

    struct Seen { LONGLONG create; LONGLONG cpu; std::wstring name; };
    std::map<ULONG_PTR, Seen> g_procs;
    std::vector<BYTE> g_buf(256 * 1024);
    ULONGLONG g_idle = 0, g_total = 0;

    std::wstring Cpu()
    {
        std::wstring s;
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user))
        {
            auto v = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
            ULONGLONG i = v(idle), t = v(kernel) + v(user);   // kernel time includes idle
            if (g_total && t > g_total) s = Fmt(L" | cpu %u%%", (unsigned)(100 - (i - g_idle) * 100 / (t - g_total)));
            g_idle = i;
            g_total = t;
        }

        ULONG need = 0;
        LONG st;
        while ((st = NtQuerySystemInformation(5 /* SystemProcessInformation */, g_buf.data(), (ULONG)g_buf.size(), &need)) ==
               (LONG)0xC0000004L)
            g_buf.resize(std::max<size_t>(need + 16384, g_buf.size() * 2));
        if (st != 0) return s + Fmt(L" | processi 0x%08lX", (unsigned long)st);

        std::map<ULONG_PTR, Seen> now;
        std::vector<std::pair<LONGLONG, std::wstring>> busy;
        for (BYTE* p = g_buf.data();;)
        {
            const ProcInfo* e = (const ProcInfo*)p;
            ULONG_PTR pid = (ULONG_PTR)e->UniqueProcessId;
            if (pid != 0)
            {
                Seen cur = { e->CreateTime.QuadPart, e->UserTime.QuadPart + e->KernelTime.QuadPart,
                             e->Name ? std::wstring(e->Name, e->NameLength / 2) : L"System" };
                auto old = g_procs.find(pid);
                LONGLONG used = old != g_procs.end() && old->second.create == cur.create ? cur.cpu - old->second.cpu
                              : g_total ? cur.cpu : 0;   // new process: everything it used counts for this minute
                if (used >= 2000000) busy.push_back({ used, cur.name });   // >= 200 ms in the minute
                now[pid] = cur;
            }
            if (!e->NextEntryOffset) break;
            p += e->NextEntryOffset;
        }
        g_procs.swap(now);
        std::sort(busy.rbegin(), busy.rend());
        for (size_t k = 0; k < busy.size() && k < 5; ++k)
            s += Fmt(k ? L", %s %.1fs" : L" | %s %.1fs", busy[k].second.c_str(), busy[k].first / 1e7);
        return s;
    }

    // ---- service ----

    void Report(DWORD state, DWORD exitCode)
    {
        if (g_console || !g_status) return;
        SERVICE_STATUS st = {};
        st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        st.dwCurrentState = state;
        st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        st.dwWin32ExitCode = exitCode;
        st.dwWaitHint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 5000 : 0;
        SetServiceStatus(g_status, &st);
    }

    DWORD WINAPI Control(DWORD control, DWORD, LPVOID, LPVOID)
    {
        if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
        {
            Report(SERVICE_STOP_PENDING, 0);
            SetEvent(g_stop);
            return NO_ERROR;
        }
        return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
    }

    void Run()
    {
        WSADATA wsa;
        int ws = WSAStartup(MAKEWORD(2, 2), &wsa);
        Log(Fmt(L"avvio sonda batteria 1.1, %s, pid %lu", g_console ? L"console" : L"servizio", GetCurrentProcessId()));
        OpenBattery();
        Cpu();   // first reading: baseline for the per-minute deltas
        ULONGLONG nextFlush = 0;
        for (int n = 0;; ++n)
        {
            if (n) Log(Battery() + Cpu());
            if (GetTickCount64() >= nextFlush)
            {
                Flush();
                nextFlush = GetTickCount64() + FlushMs;
            }
            if (g_console && n >= 2) break;   // console: a short smoke run
            if (WaitForSingleObject(g_stop, g_console ? 5000 : SampleMs) == WAIT_OBJECT_0) break;
        }
        Log(L"fine, " + Battery());
        Flush();
        if (ws == 0) WSACleanup();
    }

    void WINAPI ServiceMain(DWORD, LPWSTR*)
    {
        g_status = RegisterServiceCtrlHandlerExW(ServiceName, Control, nullptr);
        if (!g_status) return;
        Report(SERVICE_START_PENDING, 0);
        Report(SERVICE_RUNNING, 0);
        Run();
        Report(SERVICE_STOPPED, 0);
    }
}

int wmain(int argc, wchar_t** argv)
{
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (argc > 1 && wcscmp(argv[1], L"console") == 0)
    {
        g_console = true;
        Run();
        return 0;
    }
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<wchar_t*>(ServiceName), ServiceMain }, { nullptr, nullptr } };
    if (!StartServiceCtrlDispatcherW(table))
    {
        g_console = true;   // started outside the SCM (e.g. from Esplora): run the smoke test
        Run();
    }
    return 0;
}
