#include "pch.h"
#include "Alarms.h"
#include <windows.h>

namespace
{
// slot GUIDs of the built-in Alarms app (Alarms_NoUI.exe .data 0x9048)
const GUID SlotIds[32] = {
    {0xf6baf232, 0x298f, 0x4df7, {0x96, 0xaa, 0x18, 0x9e, 0x4f, 0x53, 0xea, 0x56}},
    {0x6dde298f, 0x5165, 0x4453, {0xbd, 0xc2, 0x7a, 0x63, 0x9e, 0xb8, 0xa1, 0x60}},
    {0x01cdd6e0, 0xd397, 0x43fd, {0x93, 0xed, 0x76, 0xf5, 0x32, 0xfc, 0x55, 0x6d}},
    {0xe359b8b8, 0xb911, 0x4c32, {0x95, 0x02, 0xf0, 0xd6, 0x12, 0x04, 0xf5, 0x93}},
    {0x3bd8b9b6, 0x7ecc, 0x43ef, {0x81, 0xf0, 0x1f, 0x49, 0x67, 0x2a, 0x78, 0xaa}},
    {0xfb55788f, 0x69f2, 0x4220, {0x83, 0x73, 0xb4, 0x3f, 0x82, 0xf6, 0xb2, 0x41}},
    {0xf9702f8c, 0x1eb2, 0x4119, {0xa2, 0x6b, 0x58, 0x20, 0x0e, 0x2e, 0x14, 0x06}},
    {0xf507b14b, 0x4bbb, 0x44c5, {0xb1, 0x9a, 0x92, 0x5a, 0xc0, 0xd4, 0xdd, 0x0c}},
    {0xc47fe1ab, 0x4c05, 0x44e8, {0x83, 0x89, 0x75, 0x8d, 0x14, 0x34, 0x9f, 0xe5}},
    {0x1d74e2a0, 0x7776, 0x46c6, {0x8a, 0x7d, 0xa7, 0x31, 0xd1, 0x7e, 0xcc, 0x00}},
    {0x4e62d43b, 0x62c8, 0x4cd9, {0xba, 0xbb, 0x2e, 0x5c, 0xea, 0x4e, 0x00, 0x84}},
    {0x67003880, 0xf516, 0x4d28, {0x9c, 0xa5, 0x57, 0x29, 0xec, 0xd0, 0xff, 0xa8}},
    {0x1d3db46c, 0xaae3, 0x4954, {0xbe, 0xef, 0xc2, 0xa2, 0x91, 0x63, 0x9c, 0x53}},
    {0x3600bbf8, 0x97b1, 0x45c1, {0x89, 0xb5, 0x3c, 0x64, 0x20, 0xc9, 0x2f, 0x2d}},
    {0xcbc02069, 0x21c0, 0x411b, {0x89, 0x27, 0x4b, 0xce, 0xc0, 0x6e, 0xc3, 0xc6}},
    {0x75c8c7b5, 0xac36, 0x411b, {0xbd, 0x30, 0xb6, 0xc1, 0xa4, 0x5b, 0x2f, 0xe2}},
    {0xf8540795, 0x833b, 0x418a, {0x91, 0xf3, 0xcd, 0xe2, 0xbb, 0x83, 0x34, 0xd0}},
    {0x121e4215, 0x65a5, 0x4761, {0xa9, 0xb8, 0xab, 0xd4, 0x4a, 0x75, 0x6d, 0x4a}},
    {0x61fc4cb5, 0x694a, 0x4a82, {0x89, 0xc0, 0x63, 0x66, 0x5e, 0xa8, 0x23, 0xa0}},
    {0x13f3064d, 0xc6eb, 0x4842, {0xad, 0xf2, 0x15, 0xc5, 0xca, 0xef, 0x49, 0x57}},
    {0x5fdb52a2, 0x59c1, 0x439a, {0xad, 0x48, 0x0d, 0x3b, 0xbc, 0x2f, 0xc5, 0x8a}},
    {0x81711245, 0xd73a, 0x43d5, {0x9e, 0x91, 0xdc, 0x8f, 0xd9, 0x5d, 0x5e, 0x54}},
    {0xc733bc4c, 0x5d4b, 0x41b2, {0x9e, 0x1b, 0x9c, 0x25, 0x7f, 0x99, 0x94, 0x31}},
    {0x82677424, 0x7c7b, 0x47b5, {0xa2, 0x28, 0xcf, 0x79, 0x10, 0xcd, 0x1d, 0xae}},
    {0x7abfea60, 0xa652, 0x4bcd, {0xa4, 0x16, 0xac, 0x8d, 0xe1, 0x7f, 0x06, 0x1c}},
    {0xa0246d3f, 0xb29b, 0x461a, {0xbf, 0xe8, 0x0a, 0xde, 0xe7, 0xea, 0xc2, 0x5e}},
    {0xa8bf6a3f, 0x0699, 0x41a1, {0xaa, 0x54, 0x1b, 0xd0, 0xa9, 0xf0, 0x19, 0x45}},
    {0x5d2554d2, 0x0c87, 0x45be, {0x8d, 0x7a, 0x02, 0x96, 0xf5, 0x15, 0xc7, 0x12}},
    {0x3c1857e2, 0x1b3f, 0x436c, {0xa6, 0x6d, 0xa6, 0x0c, 0x0c, 0xa1, 0xbf, 0x11}},
    {0x2071c0d1, 0x2bbe, 0x48e8, {0xba, 0x86, 0xdd, 0x89, 0x03, 0x64, 0x96, 0xce}},
    {0x33506c9d, 0xe665, 0x4a67, {0x9a, 0x63, 0xf4, 0x16, 0xfd, 0x84, 0x74, 0xa5}},
    {0x72309536, 0x68a5, 0x4380, {0x9a, 0xe9, 0x12, 0x19, 0x7f, 0x77, 0xff, 0x8e}},
};

// Shell_CreateReminder input (0xa8 bytes, built by Alarms_NoUI f_3c28)
struct ReminderIn
{
    GUID id;
    DWORD flags;          // 0x200 = disabled
    DWORD one;            // 1
    SYSTEMTIME start;     // local, seconds = 0
    SYSTEMTIME start2;    // same
    LPCWSTR category;     // L"Alarms": the full-screen alarm with snooze
    LPCWSTR name;
    LPCWSTR text;         // "<time>\n<name>"
    LPCWSTR sound;
    BYTE* days;
    DWORD valid;          // 1
    DWORD five;           // 5
    BYTE rest[0xa8 - 0x54];
};
// second argument: what runs when the alarm is dismissed
struct ReminderExtra
{
    LPCWSTR program;      // required by Shell_CreateReminder (or the two pointers at +8/+0xc)
    LPCWSTR dismissCommand;
    DWORD more[3];
};
// Shell_GetReminderData output (fields read by Alarms_NoUI f_3de8)
struct ReminderOut
{
    BYTE head[0x18];
    SYSTEMTIME start;
    BYTE mid[0x3c - 0x28];
    LPCWSTR name;
    LPCWSTR text;
    LPCWSTR sound;
    BYTE* days;
    DWORD valid;
    BYTE tail[0xbc - 0x50];
    DWORD enabled;
};

static_assert(sizeof(ReminderIn) == 0xa8, "ReminderIn layout");
static_assert(offsetof(ReminderOut, enabled) == 0xbc, "ReminderOut layout");

typedef HRESULT(WINAPI* PInit)(HANDLE*);
typedef HRESULT(WINAPI* PDeInit)(HANDLE);
typedef HRESULT(WINAPI* PCreate)(HANDLE, ReminderIn*, ReminderExtra*);
typedef HRESULT(WINAPI* PGet)(HANDLE, const GUID*, ReminderOut**);
typedef HRESULT(WINAPI* PRelease)(HANDLE, ReminderOut*);
typedef HRESULT(WINAPI* PDelete)(HANDLE, const GUID*);
typedef HMODULE(WINAPI* PLoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
typedef LONG(WINAPI* PRegOpenKeyExW)(HKEY, LPCWSTR, DWORD, DWORD, HKEY*);
typedef LONG(WINAPI* PRegQueryValueExW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI* PRegCloseKey)(HKEY);

// kernelbase holds the functions outside the app partition (LoadLibraryExW, registry)
HMODULE KernelBase()
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(&VirtualQuery), &mbi, sizeof(mbi)) == 0) return nullptr;
    return static_cast<HMODULE>(mbi.AllocationBase);
}

std::wstring Hex(HRESULT hr)
{
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
    return b;
}

struct Api
{
    PInit init = nullptr;
    PDeInit deinit = nullptr;
    PCreate create = nullptr;
    PGet get = nullptr;
    PRelease release = nullptr;
    PDelete remove = nullptr;
    HANDLE handle = nullptr;

    bool Open(std::wstring& error)
    {
        HMODULE kb = KernelBase();
        auto load = kb ? reinterpret_cast<PLoadLibraryExW>(GetProcAddress(kb, "LoadLibraryExW")) : nullptr;
        HMODULE dll = load ? load(L"ShellChromeAPI.dll", nullptr, 0) : nullptr;
        if (!dll)
        {
            error = L"ShellChromeAPI.dll non caricata (" + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        init = reinterpret_cast<PInit>(GetProcAddress(dll, "Shell_InitReminders"));
        deinit = reinterpret_cast<PDeInit>(GetProcAddress(dll, "Shell_DeInitReminders"));
        create = reinterpret_cast<PCreate>(GetProcAddress(dll, "Shell_CreateReminder"));
        get = reinterpret_cast<PGet>(GetProcAddress(dll, "Shell_GetReminderData"));
        release = reinterpret_cast<PRelease>(GetProcAddress(dll, "Shell_ReleaseReminderData"));
        remove = reinterpret_cast<PDelete>(GetProcAddress(dll, "Shell_DeleteReminder"));
        if (!init || !deinit || !create || !get || !release || !remove)
        {
            error = L"funzioni delle sveglie assenti";
            return false;
        }
        HRESULT hr = init(&handle);
        if (FAILED(hr))
        {
            error = L"Shell_InitReminders " + Hex(hr);
            handle = nullptr;
            return false;
        }
        return true;
    }

    ~Api()
    {
        if (handle) deinit(handle);
    }
};

// the alarm sound chosen in Settings (HKLM\...\SoundCategories\Alarm DefaultSound), like Alarms_NoUI f_7320
std::wstring DefaultSound()
{
    std::wstring fallback = L"C:\\Programs\\CommonFiles\\Sounds\\Alarm-01.wma";
    HMODULE kb = KernelBase();
    if (!kb) return fallback;
    auto open = reinterpret_cast<PRegOpenKeyExW>(GetProcAddress(kb, "RegOpenKeyExW"));
    auto query = reinterpret_cast<PRegQueryValueExW>(GetProcAddress(kb, "RegQueryValueExW"));
    auto close = reinterpret_cast<PRegCloseKey>(GetProcAddress(kb, "RegCloseKey"));
    if (!open || !query || !close) return fallback;
    HKEY key = nullptr;
    if (open(reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x80000002)),
             L"Software\\Microsoft\\EventSounds\\SoundCategories\\Alarm", 0, 0x20019 /* KEY_READ */, &key) != 0)
        return fallback;
    wchar_t buffer[262] = {};
    DWORD type = 0, size = sizeof(buffer) - sizeof(wchar_t);
    LONG r = query(key, L"DefaultSound", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    close(key);
    return r == 0 && type == 1 /* REG_SZ */ && buffer[0] ? std::wstring(buffer) : fallback;
}

unsigned long long ToTicks(const SYSTEMTIME& st)
{
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

SYSTEMTIME FromTicks(unsigned long long ticks)
{
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    return st;
}

// next local time at hour:minute on a day allowed by the mask (bit0 = Monday), from now
SYSTEMTIME NextRing(int hour, int minute, unsigned char days)
{
    const unsigned long long day = 864000000000ULL;
    SYSTEMTIME now;
    GetLocalTime(&now);
    SYSTEMTIME at = now;
    at.wHour = static_cast<WORD>(hour);
    at.wMinute = static_cast<WORD>(minute);
    at.wSecond = 0;
    at.wMilliseconds = 0;
    unsigned long long base = ToTicks(at), current = ToTicks(now);
    for (int i = 0; i < 8; ++i)
    {
        unsigned long long t = base + day * i;
        if (t <= current) continue;
        SYSTEMTIME st = FromTicks(t);
        int bit = st.wDayOfWeek == 0 ? 6 : st.wDayOfWeek - 1;
        if (days == 0 || (days & (1 << bit))) return st;
    }
    return FromTicks(base + day);
}
}  // namespace

std::vector<AlarmInfo> Alarms::List(std::wstring& error)
{
    std::vector<AlarmInfo> result;
    Api api;
    if (!api.Open(error)) return result;
    for (int slot = 0; slot < 32; ++slot)
    {
        ReminderOut* data = nullptr;
        if (FAILED(api.get(api.handle, &SlotIds[slot], &data)) || !data) continue;
        if (data->valid >= 1)
        {
            AlarmInfo a;
            a.slot = slot;
            a.hour = data->start.wHour;
            a.minute = data->start.wMinute;
            a.year = data->start.wYear;
            a.month = data->start.wMonth;
            a.day = data->start.wDay;
            a.days = data->days ? static_cast<unsigned char>(*data->days & 0x7f) : 0;
            a.enabled = data->enabled != 0;
            a.label = data->name ? data->name : L"";
            result.push_back(a);
        }
        api.release(api.handle, data);
    }
    return result;
}

int Alarms::Save(AlarmInfo info, std::wstring& error)
{
    Api api;
    if (!api.Open(error)) return -1;
    if (info.slot < 0 || info.slot > 31)
    {
        bool used[32] = {};
        std::wstring ignored;
        for (const AlarmInfo& a : List(ignored)) used[a.slot] = true;
        info.slot = -1;
        for (int i = 0; i < 32 && info.slot < 0; ++i)
            if (!used[i]) info.slot = i;
        if (info.slot < 0)
        {
            error = L"ci sono gi\u00e0 32 sveglie";
            return -1;
        }
    }

    SYSTEMTIME at;
    if (info.year != 0 && info.days == 0)
    {
        at = {};
        at.wYear = static_cast<WORD>(info.year);
        at.wMonth = static_cast<WORD>(info.month);
        at.wDay = static_cast<WORD>(info.day);
        at.wHour = static_cast<WORD>(info.hour);
        at.wMinute = static_cast<WORD>(info.minute);
        at = FromTicks(ToTicks(at));  // fills wDayOfWeek
    }
    else
    {
        at = NextRing(info.hour, info.minute, info.days);
    }

    std::wstring name = info.label.empty() ? L"Sveglia" : info.label;
    wchar_t time[8];
    swprintf_s(time, L"%02d:%02d", info.hour, info.minute);
    std::wstring text = std::wstring(time) + L"\n" + name;
    std::wstring sound = DefaultSound();
    const wchar_t* program = L"c:\\Programs\\Alarms\\Alarms_NoUI.exe";
    std::wstring dismiss = std::wstring(program) + L" ReminderDismissed " + std::to_wstring(info.slot);
    BYTE days = info.days & 0x7f;

    ReminderIn in = {};
    in.id = SlotIds[info.slot];
    in.flags = info.enabled ? 0 : 0x200;
    in.one = 1;
    in.start = at;
    in.start2 = at;
    in.category = L"Alarms";
    in.name = name.c_str();
    in.text = text.c_str();
    in.sound = sound.c_str();
    in.days = &days;
    in.valid = 1;
    in.five = 5;
    ReminderExtra extra = {};
    extra.program = program;
    extra.dismissCommand = dismiss.c_str();

    HRESULT hr = api.create(api.handle, &in, &extra);
    if (FAILED(hr))
    {
        error = L"Shell_CreateReminder " + Hex(hr);
        return -1;
    }
    return info.slot;
}

bool Alarms::Remove(int slot, std::wstring& error)
{
    if (slot < 0 || slot > 31) return false;
    Api api;
    if (!api.Open(error)) return false;
    HRESULT hr = api.remove(api.handle, &SlotIds[slot]);
    if (FAILED(hr))
    {
        error = L"Shell_DeleteReminder " + Hex(hr);
        return false;
    }
    return true;
}
