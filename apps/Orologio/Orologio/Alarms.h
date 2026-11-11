// Alarms: real system alarms (they ring with the app closed, show on the lock screen, snooze) through the
// reminder API of ShellChromeAPI.dll, the same calls and the same 32 slot GUIDs as the built-in Alarms app
// (reverse engineered from C:\Programs\Alarms\Alarms_NoUI.exe), so they also appear in that app.
#pragma once

#include <string>
#include <vector>

struct AlarmInfo
{
    int slot;              // 0..31
    int hour, minute;
    int year, month, day;  // next ring date (local)
    unsigned char days;    // repeat: bit0 = Monday ... bit6 = Sunday; 0 = once
    bool enabled;
    std::wstring label;
};

class Alarms
{
public:
    // all alarms in the system slots; error gets the failure text (empty when ok)
    static std::vector<AlarmInfo> List(std::wstring& error);
    // creates or replaces the alarm in info.slot (-1 = first free slot); the date is computed from
    // hour/minute/days unless year != 0 (a one-off alarm on that day). Returns the slot or -1.
    static int Save(AlarmInfo info, std::wstring& error);
    static bool Remove(int slot, std::wstring& error);
};
