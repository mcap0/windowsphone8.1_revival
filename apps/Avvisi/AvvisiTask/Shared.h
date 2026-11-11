#pragma once

// Helpers shared by the two background tasks (and read by the app through LocalSettings / avvisi.log).
// Not part of the component's metadata: plain C++ functions.

#include <string>

namespace AvvisiTask
{
    // 100 ns units since 1601 (UTC), like FILETIME.
    long long NowUtc();
    std::wstring LocalClock();          // "HH:MM:SS"

    // One line to LocalFolder\avvisi.log (shared by all the processes of the package) and, best effort,
    // to the PC inbox (http://192.168.1.148:8765/avvisi) when the phone is on the home Wi-Fi.
    void Log(const std::wstring& text);

    // Toast with fallbacks: the background audio task gets WPN_E_INVALID_APP from the plain notifier, so other
    // notifiers / a scheduled toast are tried in turn; the way that worked is remembered ("toastMode").
    void Toast(const std::wstring& title, const std::wstring& text, bool silent, const std::wstring& launch);
    // Which process is showing toasts ("audio", "watch"): each remembers its own working way.
    void SetToastContext(const wchar_t* name);

    // Cancels an async operation after the given seconds (a waiting .get() then throws task_canceled).
    Windows::System::Threading::ThreadPoolTimer^ CancelAfter(Windows::Foundation::IAsyncInfo^ info, int seconds);

    int BatteryPercent();
    std::wstring NetworkText();         // "wifi", "cell", "nessuna", "... (limitata)"
    std::wstring MemoryText();          // "1234 KB / 40960 KB"
    std::wstring Hex(int value);

    // Status for the app, in LocalSettings.
    void SetStatus(const std::wstring& state);
    void SetTick(unsigned int ticks, long long started, int batteryStart, int battery);
    // Last result of each check, for the app's panel ("tgStatus", "mailStatus", "checkedAt").
    void SetChecks(const std::wstring& telegram, const std::wstring& mail);
}
