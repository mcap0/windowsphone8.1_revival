#include "pch.h"
#include "Agenda.h"
#include "Logger.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel::Appointments;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Globalization;

namespace {

const wchar_t* const CalendarName = L"Telefono";

// local midnight of "AAAA-MM-GG" (empty or invalid = today), optionally at hour:minute
bool LocalMoment(const std::wstring& date, int hour, int minute, DateTime& out) {
    auto cal = ref new Calendar();
    cal->SetToNow();
    int y = 0, m = 0, d = 0;
    if (!date.empty()) {
        if (swscanf_s(date.c_str(), L"%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
        cal->Year = y;
        cal->Month = m;
        cal->Day = d;
    }
    cal->ChangeClock(L"24HourClock");  // Hour 0..23, a single period
    cal->Period = 1;
    cal->Hour = hour;
    cal->Minute = minute;
    cal->Second = 0;
    cal->Nanosecond = 0;
    out = cal->GetDateTime();
    return true;
}

std::wstring LocalText(DateTime when) {
    auto cal = ref new Calendar();
    cal->ChangeClock(L"24HourClock");
    cal->SetDateTime(when);
    wchar_t b[32];
    swprintf_s(b, L"%04d-%02d-%02d %02d:%02d", cal->Year, cal->Month, cal->Day, cal->Hour, cal->Minute);
    return b;
}

JsonObject^ Error(const std::wstring& text) {
    JsonObject^ o = ref new JsonObject();
    o->SetNamedValue(L"errore", JsonValue::CreateStringValue(ref new String(text.c_str())));
    return o;
}

IJsonValue^ Text(String^ s) { return JsonValue::CreateStringValue(s == nullptr ? L"" : s); }

}  // namespace

task<JsonObject^> Agenda::Read(const std::wstring& from, int days) {
    DateTime start;
    if (!LocalMoment(from, 0, 0, start)) return task_from_result(Error(L"data non valida: " + from));
    if (days < 1) days = 1;
    if (days > 31) days = 31;
    TimeSpan span;
    span.Duration = static_cast<long long>(days) * 24 * 3600 * 10000000LL;
    return create_task(AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AllCalendarsReadOnly))
        .then([start, span](AppointmentStore^ store) {
            auto options = ref new FindAppointmentsOptions();
            options->MaxCount = 100;
            options->FetchProperties->Append(AppointmentProperties::Subject);
            options->FetchProperties->Append(AppointmentProperties::Location);
            options->FetchProperties->Append(AppointmentProperties::StartTime);
            options->FetchProperties->Append(AppointmentProperties::Duration);
            options->FetchProperties->Append(AppointmentProperties::AllDay);
            return create_task(store->FindAppointmentsAsync(start, span, options));
        })
        .then([](task<IVectorView<Appointment^>^> t) {
            try {
                auto list = t.get();
                JsonArray^ events = ref new JsonArray();
                for (unsigned i = 0; i < list->Size; ++i) {
                    Appointment^ a = list->GetAt(i);
                    JsonObject^ e = ref new JsonObject();
                    e->SetNamedValue(L"titolo", Text(a->Subject));
                    e->SetNamedValue(L"inizio", Text(ref new String(LocalText(a->StartTime).c_str())));
                    e->SetNamedValue(L"tutto_il_giorno", JsonValue::CreateBooleanValue(a->AllDay));
                    e->SetNamedValue(L"durata_minuti", JsonValue::CreateNumberValue(
                        static_cast<double>(a->Duration.Duration / 600000000LL)));
                    if (a->Location != nullptr && !a->Location->IsEmpty()) e->SetNamedValue(L"luogo", Text(a->Location));
                    events->Append(e);
                }
                JsonObject^ out = ref new JsonObject();
                out->SetNamedValue(L"eventi", events);
                return out;
            } catch (Exception^ ex) {
                Logger::Write(L"calendario: " + std::wstring(ex->Message->Data()));
                return Error(L"calendario non leggibile: " + std::wstring(ex->Message->Data()));
            }
        });
}

task<std::wstring> Agenda::Add(const std::wstring& title, const std::wstring& date, const std::wstring& time,
                               int minutes, const std::wstring& place) {
    int hour = 0, minute = 0;
    bool allDay = time.empty();
    if (!allDay && swscanf_s(time.c_str(), L"%d:%d", &hour, &minute) < 1)
        return task_from_result(std::wstring(L"Non ho capito l'ora dell'evento."));
    DateTime start;
    if (!LocalMoment(date, hour, minute, start)) return task_from_result(std::wstring(L"Non ho capito il giorno dell'evento."));
    auto appointment = ref new Appointment();
    appointment->Subject = ref new String(title.c_str());
    appointment->StartTime = start;
    appointment->AllDay = allDay;
    TimeSpan duration;
    duration.Duration = (allDay ? 24 * 60 : (minutes > 0 ? minutes : 60)) * 600000000LL;
    appointment->Duration = duration;
    if (!place.empty()) appointment->Location = ref new String(place.c_str());
    TimeSpan reminder;
    reminder.Duration = (allDay ? 0 : 15) * 600000000LL;
    appointment->Reminder = reminder;

    return create_task(AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AppCalendarsReadWrite))
        .then([](AppointmentStore^ store) {
            return create_task(store->FindAppointmentCalendarsAsync()).then([store](IVectorView<AppointmentCalendar^>^ list) {
                for (unsigned i = 0; i < list->Size; ++i)
                    if (list->GetAt(i)->DisplayName == ref new String(CalendarName)) return task_from_result(list->GetAt(i));
                return create_task(store->CreateAppointmentCalendarAsync(ref new String(CalendarName)));
            });
        })
        .then([appointment](AppointmentCalendar^ calendar) { return create_task(calendar->SaveAppointmentAsync(appointment)); })
        .then([title](task<void> t) {
            try {
                t.get();
                return L"Ho aggiunto \"" + title + L"\" al calendario.";
            } catch (Exception^ ex) {
                Logger::Write(L"evento: " + std::wstring(ex->Message->Data()));
                return std::wstring(L"Non sono riuscito ad aggiungere l'evento.");
            }
        });
}
