// Agenda: the phone calendars (Google ones included) for the assistant: read a span of days, add an event.
#pragma once

#include <string>

class Agenda {
public:
    // events from the start of day "AAAA-MM-GG" (empty = today) for days days:
    // {"eventi": [{"titolo", "inizio": "AAAA-MM-GG HH:MM", "tutto_il_giorno", "durata_minuti", "luogo"}]} or {"errore"}
    static concurrency::task<Windows::Data::Json::JsonObject^> Read(const std::wstring& from, int days);
    // in the assistant's own calendar "Telefono" (apps can only write their own calendars); time empty = all day.
    // Returns the sentence for the user.
    static concurrency::task<std::wstring> Add(const std::wstring& title, const std::wstring& date,
                                               const std::wstring& time, int minutes, const std::wstring& place);
};
