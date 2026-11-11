// Actions: what the assistant does on the phone for each function call of the command network. Each returns a short Italian
// sentence for the user (and the log); failures say what went wrong.
#pragma once

#include <string>
#include <vector>

#include "Core/Text.h"

class Actions {
public:
    static concurrency::task<std::wstring> Timer(int seconds, const std::wstring& label);
    // local time; days = repeat mask (bit0 Monday ... bit6 Sunday, 0 = once). Our Orologio creates a system
    // alarm through orologio:sveglia?ora=HH:MM&giorno=AAAA-MM-GG|giorni=mask&etichetta=...
    static concurrency::task<std::wstring> Alarm(int year, int month, int day, int hour, int minute, int days,
                                                 const std::wstring& label);
    // OneNote "Unfiled notes" through System32\CaptureNote.dll (what Cortana used), saved in the background
    static std::wstring Note(const std::wstring& title, const std::wstring& text);
    static concurrency::task<std::wstring> Music(const std::wstring& query, const std::wstring& kind, bool shuffle);
    static concurrency::task<std::wstring> MusicControl(const std::wstring& action);
    static concurrency::task<std::wstring> WebSearch(const std::wstring& query);

    // a settings page or one of our apps, by the names in OpenTargets()
    static concurrency::task<std::wstring> Open(const std::wstring& what);
    // our Meteo app on a place (empty = phone position) and a day AAAA-MM-GG (empty = today)
    static concurrency::task<std::wstring> Weather(const std::wstring& place, const std::wstring& day);
    static std::wstring OpenTargets();
    // number, or a name searched (loosely) in the contacts: only chooses the number; the page then shows the phone's
    // call screen through TakePendingCall, after the answer is on screen
    static concurrency::task<std::wstring> Call(const std::wstring& who);
    static bool TakePendingCall(std::wstring& number, std::wstring& name);
    // a name heard only approximately: the likely contacts (one row per number), for the page to list
    struct CallChoice { std::wstring name, number, kind; };
    static std::vector<CallChoice> TakeCallChoices();
    // scheduled notification with sound (works with the app closed and after a restart)
    static std::wstring Reminder(const std::wstring& text, const assist::LocalTime& when);
    // the system alarms (same as Orologio's "sveglie" tab)
    static Windows::Data::Json::JsonObject^ ListAlarms();
    static std::wstring RemoveAlarm(int hour, int minute);
    // the timer started by voice (Timer() remembers when it ends)
    static Windows::Data::Json::JsonObject^ TimerStatus();
    static concurrency::task<std::wstring> CancelTimer();
};
