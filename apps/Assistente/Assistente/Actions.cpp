#include "pch.h"
#include "Actions.h"

#include <algorithm>
#include <vector>
#include "Logger.h"
#include "Core/Text.h"
#include "../../Orologio/Orologio/Alarms.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Data::Json;
using namespace Windows::Storage;
using namespace Windows::System;

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // ASCII only

task<std::wstring> Launch(const std::string& uri, const std::wstring& ok, const std::wstring& fail) {
    Logger::Write(L"apro " + Wide(uri));
    return create_task(Launcher::LaunchUriAsync(ref new Uri(ref new String(Wide(uri).c_str()))))
        .then([ok, fail](task<bool> t) -> std::wstring {
            try {
                return t.get() ? ok : fail;
            } catch (Exception^) {
                return fail;
            }
        });
}

// kernelbase is the module that contains VirtualQuery: from there the functions outside the app partition
HMODULE KernelBase() {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(&VirtualQuery), &mbi, sizeof(mbi)) == 0) return nullptr;
    return static_cast<HMODULE>(mbi.AllocationBase);
}

typedef HMODULE(WINAPI* PLoadLibraryExW)(LPCWSTR, HANDLE, DWORD);

// CaptureNote.dll's content creator (vtable reverse engineered from SpeechUX.dll's OneNoteHelper):
// [3] GetTextStyler [4] AddText(text, langid, 0) [5] AddEmbeddedFile [6] AddTitle(text, langid)
// [7] GetTaskUri(mode, buffer, size) [8] CaptureNewNote(2, 0, 1) = save in the background
struct IContentCreator : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetTextStyler(void** styler) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddText(LPCWSTR text, WORD langId, void* styler) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddEmbeddedFile(LPCWSTR path, WORD langId) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddTitle(LPCWSTR text, WORD langId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTaskUri(int mode, LPWSTR buffer, int size) = 0;
    virtual HRESULT STDMETHODCALLTYPE CaptureNewNote(int mode, int a, int b) = 0;
};
typedef HRESULT(WINAPI* PGetContentCreator)(IContentCreator** out);

std::wstring Hex(HRESULT hr) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
    return b;
}

}  // namespace

task<std::wstring> Actions::Timer(int seconds, const std::wstring& label) {
    if (seconds <= 0 || seconds > 86399) return task_from_result(std::wstring(L"Durata del timer non valida."));
    std::string uri = "orologio:timer?secondi=" + std::to_string(seconds);
    if (!label.empty()) uri += "&etichetta=" + assist::PercentEncode(Utf8(label));
    int m = seconds / 60, s = seconds % 60;
    std::wstring what = m > 0 ? std::to_wstring(m) + (m == 1 ? L" minuto" : L" minuti") : std::wstring();
    if (s > 0) what += (what.empty() ? L"" : L" e ") + std::to_wstring(s) + L" secondi";
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long end = ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) +
                             static_cast<unsigned long long>(seconds) * 10000000ULL;
    ApplicationData::Current->LocalSettings->Values->Insert(L"timer.fine", end);
    return Launch(uri, L"Timer di " + what + L" avviato.", L"Non riesco ad aprire l'Orologio.");
}

task<std::wstring> Actions::Alarm(int year, int month, int day, int hour, int minute, int days,
                                  const std::wstring& label) {
    char when[64];
    if (days != 0)
        sprintf_s(when, "ora=%02d:%02d&giorni=%d", hour, minute, days);
    else
        sprintf_s(when, "ora=%02d:%02d&giorno=%04d-%02d-%02d", hour, minute, year, month, day);
    std::string uri = std::string("orologio:sveglia?") + when;
    if (!label.empty()) uri += "&etichetta=" + assist::PercentEncode(Utf8(label));
    wchar_t time[8];
    swprintf_s(time, L"%02d:%02d", hour, minute);
    return Launch(uri, std::wstring(L"Sveglia impostata per le ") + time + L".", L"Non riesco ad aprire l'Orologio.");
}

std::wstring Actions::Note(const std::wstring& title, const std::wstring& text) {
    HMODULE kb = KernelBase();
    PLoadLibraryExW load = kb ? reinterpret_cast<PLoadLibraryExW>(GetProcAddress(kb, "LoadLibraryExW")) : nullptr;
    HMODULE dll = load ? load(L"CaptureNote.dll", nullptr, 0) : nullptr;
    if (!dll) return L"OneNote non disponibile (CaptureNote.dll, errore " + std::to_wstring(GetLastError()) + L").";
    PGetContentCreator get = reinterpret_cast<PGetContentCreator>(GetProcAddress(dll, "GetContentCreator"));
    if (!get) return L"OneNote non disponibile (GetContentCreator).";
    IContentCreator* cc = nullptr;
    HRESULT hr = get(&cc);
    const WORD italian = 0x0410;
    if (SUCCEEDED(hr) && cc) hr = cc->AddText(text.c_str(), italian, nullptr);
    if (SUCCEEDED(hr) && cc && !title.empty()) hr = cc->AddTitle(title.c_str(), italian);
    if (SUCCEEDED(hr) && cc) hr = cc->CaptureNewNote(2, 0, 1);
    Logger::Write(L"nota: " + Hex(hr));
    if (cc) cc->Release();
    if (FAILED(hr)) return L"Non sono riuscito a salvare la nota in OneNote (" + Hex(hr) + L").";
    return L"Nota salvata in OneNote.";
}

task<std::wstring> Actions::Music(const std::wstring& query, const std::wstring& kind, bool shuffle) {
    std::string uri = "musica:riproduci?q=" + assist::PercentEncode(Utf8(query));
    if (!kind.empty()) uri += "&tipo=" + assist::PercentEncode(Utf8(kind));
    if (shuffle) uri += "&casuale=1";
    std::wstring say = query.empty() ? L"Metto musica a caso." : L"Metto " + query + (shuffle ? L" a caso." : L".");
    return Launch(uri, say, L"Non riesco ad aprire Musica.");
}

task<std::wstring> Actions::MusicControl(const std::wstring& action) {
    std::string uri = "musica:azione?a=" + assist::PercentEncode(Utf8(action));
    return Launch(uri, L"Fatto.", L"Non riesco ad aprire Musica.");
}

task<std::wstring> Actions::WebSearch(const std::wstring& query) {
    return Launch(assist::GoogleSearchUrl(Utf8(query)), L"Apro la ricerca: " + query + L".",
                  L"Non riesco ad aprire il browser.");
}

namespace {

std::wstring Lower(const std::wstring& s) {
    std::wstring r(s);
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return r;
}

// local moment -> DateTime (UTC ticks) through the calendar, so DST is right
Windows::Foundation::DateTime ToDateTime(const assist::LocalTime& t) {
    auto cal = ref new Windows::Globalization::Calendar();
    cal->ChangeClock(L"24HourClock");
    cal->SetToNow();
    cal->Year = t.year;
    cal->Month = t.month;
    cal->Day = t.day;
    cal->Period = 1;
    cal->Hour = t.hour;
    cal->Minute = t.minute;
    cal->Second = 0;
    cal->Nanosecond = 0;
    return cal->GetDateTime();
}

typedef HRESULT(WINAPI* PGeneric)(LPCWSTR, LPCWSTR, LPCWSTR, unsigned);

// opens one of our apps by AUMID (Shell_LaunchSessionGeneric of ShellChromeAPI.dll, as the Search hold patch)
bool LaunchApp(const std::wstring& aumid) {
    HMODULE kb = KernelBase();
    PLoadLibraryExW load = kb ? reinterpret_cast<PLoadLibraryExW>(GetProcAddress(kb, "LoadLibraryExW")) : nullptr;
    HMODULE api = load ? load(L"ShellChromeAPI.dll", nullptr, 0) : nullptr;
    PGeneric launch = api ? reinterpret_cast<PGeneric>(GetProcAddress(api, "?Shell_LaunchSessionGeneric@@YAJPBG00I@Z")) : nullptr;
    if (!launch) return false;
    HRESULT hr = launch(aumid.c_str(), L"", L"", 2);
    Logger::Write(L"apro " + aumid + L" -> " + Hex(hr));
    return SUCCEEDED(hr);
}

struct Target {
    const wchar_t* name;
    const wchar_t* uri;    // settings page or protocol
    const wchar_t* aumid;  // our apps without a protocol
};

const Target Targets[] = {
    {L"wi-fi", L"ms-settings-wifi:", nullptr},
    {L"bluetooth", L"ms-settings-bluetooth:", nullptr},
    {L"modalit\u00e0 aereo", L"ms-settings-airplanemode:", nullptr},
    {L"rete cellulare", L"ms-settings-cellular:", nullptr},
    {L"posizione", L"ms-settings-location:", nullptr},
    {L"risparmio batteria", L"ms-settings-power:", nullptr},
    {L"schermata di blocco", L"ms-settings-lock:", nullptr},
    {L"rotazione schermo", L"ms-settings-screenrotation:", nullptr},
    {L"account email", L"ms-settings-emailandaccounts:", nullptr},
    {L"musica", L"musica:", nullptr},
    {L"orologio", L"orologio:", nullptr},
    {L"sveglie", L"orologio:sveglie", nullptr},
    {L"calendario", nullptr, L"DumbPhone.Calendario_ry999vsxp6fxe!App"},
    {L"meteo", L"meteo:", nullptr},
    {L"telegram", nullptr, L"DumbPhone.TelegramWeb_ry999vsxp6fxe!App"},
    {L"posta", nullptr, L"DumbPhone.Posta_ry999vsxp6fxe!App"},
    {L"registratore", nullptr, L"DumbPhone.Registratore_ry999vsxp6fxe!App"},
    {L"file", nullptr, L"DumbPhone.File_ry999vsxp6fxe!App"},
    {L"perle tv", nullptr, L"DumbPhone.PerleTV_ry999vsxp6fxe!App"},
    {L"authenticator", nullptr, L"DumbPhone.Authenticator81_ry999vsxp6fxe!App"},
    {L"appunti", nullptr, L"DumbPhone.Appunti_ry999vsxp6fxe!App"},
    {L"esplora", nullptr, L"DumbPhone.Esplora_ry999vsxp6fxe!App"},
};

const wchar_t* const TimerEndKey = L"timer.fine";  // FILETIME ticks (UTC) of the timer started by voice

unsigned long long NowTicks() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

JsonObject^ ResultObject(const std::wstring& key, const std::wstring& value) {
    JsonObject^ o = ref new JsonObject();
    o->SetNamedValue(ref new String(key.c_str()), JsonValue::CreateStringValue(ref new String(value.c_str())));
    return o;
}

}  // namespace

task<std::wstring> Actions::Weather(const std::wstring& place, const std::wstring& day) {
    std::string uri = "meteo:?luogo=" + assist::PercentEncode(Utf8(place)) + "&giorno=" + assist::PercentEncode(Utf8(day));
    return Launch(uri, L"", L"Non riesco ad aprire Meteo.");
}

std::wstring Actions::OpenTargets() {
    std::wstring all;
    for (const Target& t : Targets) all += (all.empty() ? L"" : L", ") + std::wstring(t.name);
    return all;
}

task<std::wstring> Actions::Open(const std::wstring& what) {
    std::wstring w = Lower(what);
    for (const Target& t : Targets) {
        if (w != t.name) continue;
        if (t.aumid)
            return task_from_result(std::wstring(LaunchApp(t.aumid) ? L"Apro " + w + L"." : L"Non riesco ad aprire " + w + L"."));
        return Launch(Utf8(t.uri), L"Apro " + w + L".", L"Non riesco ad aprire " + w + L".");
    }
    return task_from_result(L"Non so aprire " + what + L".");
}

namespace {

// lower case, no accents, only letters, digits and single spaces: "Zia Nicolò" -> "zia nicolo"
std::wstring Fold(const std::wstring& s) {
    static const wchar_t* from = L"àáâäèéêëìíîïòóôöùúûüçñ";
    static const wchar_t* to = L"aaaaeeeeiiiioooouuuucn";
    std::wstring r;
    for (wchar_t c : s) {
        c = static_cast<wchar_t>(towlower(c));
        if (const wchar_t* f = wcschr(from, c)) c = to[f - from];
        if (iswalnum(c)) r += c;
        else if (!r.empty() && r.back() != L' ') r += L' ';
    }
    while (!r.empty() && r.back() == L' ') r.pop_back();
    return r;
}

std::vector<std::wstring> Words(const std::wstring& folded) {
    std::vector<std::wstring> w;
    size_t at = 0;
    while (at < folded.size()) {
        size_t sp = folded.find(L' ', at);
        if (sp == std::wstring::npos) sp = folded.size();
        if (sp > at) w.push_back(folded.substr(at, sp - at));
        at = sp + 1;
    }
    return w;
}

std::wstring Squash(const std::wstring& folded) {
    std::wstring r;
    for (wchar_t c : folded) if (c != L' ') r += c;
    return r;
}

size_t Distance(const std::wstring& a, const std::wstring& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t up = row[j];
            row[j] = (std::min)((std::min)(row[j] + 1, row[j - 1] + 1), diag + (a[i - 1] == b[j - 1] ? 0 : 1));
            diag = up;
        }
    }
    return row[b.size()];
}

double Similar(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return 0;
    return 1.0 - static_cast<double>(Distance(a, b)) / (std::max)(a.size(), b.size());
}

// How well a spoken name fits a contact, 0..1. Speech often glues or splits words ("Verimobile" for
// "Veri mobile") and gets a letter or two wrong, so: word by word, and the whole name without spaces.
double NameScore(const std::wstring& query, const std::wstring& name) {
    if (query.empty() || name.empty()) return 0;
    if (query == name) return 1;
    std::vector<std::wstring> q = Words(query), n = Words(name);
    double words = 0;
    for (const auto& qw : q) {
        double best = 0;
        for (const auto& nw : n) {
            double sc = qw == nw ? 1 : Similar(qw, nw);
            if (qw.size() >= 3 && nw.size() >= 3 && (qw.compare(0, nw.size(), nw) == 0 || nw.compare(0, qw.size(), qw) == 0))
                sc = (std::max)(sc, 0.85);
            best = (std::max)(best, sc);
        }
        words += best;
    }
    words /= q.size();
    if (words >= 1) words = 0.95;   // every word there ("Francesca" in "Francesca Terranova"), but not the whole name
    std::wstring qs = Squash(query), ns = Squash(name);
    double whole = Similar(qs, ns);
    if (ns.size() >= 4 && (qs.find(ns) != std::wstring::npos || ns.find(qs) != std::wstring::npos)) whole = (std::max)(whole, 0.85);
    return (std::max)(words, whole);
}

std::wstring g_callNumber, g_callName;
std::vector<Actions::CallChoice> g_choices;

}  // namespace

std::vector<Actions::CallChoice> Actions::TakeCallChoices() {
    std::vector<CallChoice> r;
    r.swap(g_choices);
    return r;
}

bool Actions::TakePendingCall(std::wstring& number, std::wstring& name) {
    if (g_callNumber.empty()) return false;
    number.swap(g_callNumber);
    name.swap(g_callName);
    g_callNumber.clear();
    g_callName.clear();
    return true;
}

task<std::wstring> Actions::Call(const std::wstring& who) {
    std::wstring digits;
    for (wchar_t c : who) if (iswdigit(c) || c == L'+') digits += c;
    bool isNumber = digits.size() >= 3 && digits.size() * 2 >= who.size();
    if (isNumber) {
        g_callNumber = digits;
        g_callName.clear();
        return task_from_result(L"Chiamo il " + digits + L".");
    }
    // "cellulare", "casa", "lavoro" pick the number, the rest is the name
    using namespace Windows::ApplicationModel::Contacts;
    std::wstring query;
    int kind = -1;
    for (const auto& w : Words(Fold(who))) {
        if (w == L"cellulare" || w == L"cell" || w == L"mobile" || w == L"telefonino") kind = static_cast<int>(ContactPhoneKind::Mobile);
        else if (w == L"casa") kind = static_cast<int>(ContactPhoneKind::Home);
        else if (w == L"lavoro" || w == L"ufficio") kind = static_cast<int>(ContactPhoneKind::Work);
        else query += (query.empty() ? L"" : L" ") + w;
    }
    if (query.empty()) query = Fold(who);
    // all the contacts: FindContactsAsync(text) only finds exact prefixes, not "Verimobile" for "Veri"
    return create_task(ContactManager::RequestStoreAsync())
        .then([](ContactStore^ store) { return create_task(store->FindContactsAsync()); })
        .then([who, query, kind](task<Windows::Foundation::Collections::IVectorView<Contact^>^> t) {
            try {
                auto list = t.get();
                struct Hit { Contact^ c; double score; };
                std::vector<Hit> hits;
                for (unsigned i = 0; i < list->Size; ++i) {
                    Contact^ c = list->GetAt(i);
                    if (c->Phones->Size == 0) continue;
                    double sc = NameScore(query, Fold(c->DisplayName->Data()));
                    std::wstring first = c->FirstName != nullptr ? c->FirstName->Data() : L"";
                    std::wstring last = c->LastName != nullptr ? c->LastName->Data() : L"";
                    if (!first.empty() || !last.empty()) {
                        sc = (std::max)(sc, NameScore(query, Fold(first + L" " + last)));
                        sc = (std::max)(sc, NameScore(query, Fold(last + L" " + first)));
                    }
                    hits.push_back(Hit{ c, sc });
                }
                std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
                Logger::Write(L"contatti: " + std::to_wstring(list->Size) + L", cerco \"" + query + L"\"" +
                              (hits.empty() ? L"" : L", migliore " + std::wstring(hits[0].c->DisplayName->Data()) + L" " +
                                                    std::to_wstring(static_cast<int>(hits[0].score * 100))));
                // only one contact with exactly that name, and no other containing it ("Mamma" vs "Mamma Rossi"), calls at
                // once; anything else shows the likely contacts to tap 
                if (hits.empty() || hits[0].score < 1 || (hits.size() > 1 && hits[1].score >= 0.95)) {
                    g_choices.clear();
                    for (size_t i = 0; i < hits.size() && i < 6 && hits[i].score >= 0.4; ++i) {
                        Contact^ c = hits[i].c;
                        for (unsigned k = 0; k < c->Phones->Size; ++k) {
                            ContactPhone^ ph = c->Phones->GetAt(k);
                            CallChoice ch;
                            ch.name = c->DisplayName->Data();
                            ch.number = ph->Number->Data();
                            ch.kind = ph->Kind == ContactPhoneKind::Mobile ? L"cellulare" : ph->Kind == ContactPhoneKind::Home ? L"casa"
                                    : ph->Kind == ContactPhoneKind::Work ? L"lavoro" : L"altro";
                            g_choices.push_back(ch);
                        }
                    }
                    if (g_choices.empty()) return L"Non trovo " + who + L" in rubrica.";
                    return std::wstring(L"Chi chiamo? Tocca il contatto.");
                }
                Contact^ best = hits[0].c;
                ContactPhone^ phone = best->Phones->GetAt(0);
                for (unsigned i = 0; i < best->Phones->Size; ++i)
                    if (static_cast<int>(best->Phones->GetAt(i)->Kind) == (kind >= 0 ? kind : static_cast<int>(ContactPhoneKind::Mobile))) {
                        phone = best->Phones->GetAt(i);
                        break;
                    }
                g_callNumber = phone->Number->Data();
                g_callName = best->DisplayName->Data();
                return L"Chiamo " + g_callName + L".";
            } catch (Exception^ ex) {
                Logger::Write(L"chiamata: " + std::wstring(ex->Message->Data()));
                return std::wstring(L"Non riesco a leggere i contatti.");
            }
        });
}

std::wstring Actions::Reminder(const std::wstring& text, const assist::LocalTime& when) {
    using namespace Windows::UI::Notifications;
    using namespace Windows::Data::Xml::Dom;
    try {
        XmlDocument^ xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        XmlNodeList^ lines = xml->GetElementsByTagName(L"text");
        lines->Item(0)->AppendChild(xml->CreateTextNode(L"Promemoria"));
        lines->Item(1)->AppendChild(xml->CreateTextNode(ref new String(text.c_str())));
        auto toast = dynamic_cast<XmlElement^>(xml->GetElementsByTagName(L"toast")->Item(0));
        auto audio = xml->CreateElement(L"audio");
        audio->SetAttribute(L"src", L"ms-winsoundevent:Notification.Reminder");
        toast->AppendChild(audio);
        auto scheduled = ref new ScheduledToastNotification(xml, ToDateTime(when));
        ToastNotificationManager::CreateToastNotifier()->AddToSchedule(scheduled);
    } catch (Exception^ ex) {
        Logger::Write(L"promemoria: " + std::wstring(ex->Message->Data()));
        return L"Non sono riuscito a creare il promemoria.";
    }
    wchar_t at[64];
    swprintf_s(at, L"il %d/%d alle %02d:%02d", when.day, when.month, when.hour, when.minute);
    return L"Ti ricorder\u00f2 " + std::wstring(at) + L": " + text + L".";
}

JsonObject^ Actions::ListAlarms() {
    std::wstring error;
    std::vector<AlarmInfo> list = Alarms::List(error);
    if (!error.empty()) return ResultObject(L"errore", error);
    JsonArray^ items = ref new JsonArray();
    for (const AlarmInfo& a : list) {
        wchar_t time[8];
        swprintf_s(time, L"%02d:%02d", a.hour, a.minute);
        JsonObject^ o = ref new JsonObject();
        o->SetNamedValue(L"ora", JsonValue::CreateStringValue(ref new String(time)));
        o->SetNamedValue(L"attiva", JsonValue::CreateBooleanValue(a.enabled));
        o->SetNamedValue(L"nome", JsonValue::CreateStringValue(ref new String(a.label.c_str())));
        o->SetNamedValue(L"giorni_ripetizione_bitmask_lun1", JsonValue::CreateNumberValue(a.days));
        items->Append(o);
    }
    JsonObject^ out = ref new JsonObject();
    out->SetNamedValue(L"sveglie", items);
    return out;
}

std::wstring Actions::RemoveAlarm(int hour, int minute) {
    std::wstring error;
    int removed = 0;
    for (const AlarmInfo& a : Alarms::List(error))
        if (a.hour == hour && a.minute == minute && Alarms::Remove(a.slot, error)) ++removed;
    wchar_t time[8];
    swprintf_s(time, L"%02d:%02d", hour, minute);
    if (removed == 0) return L"Non c'\u00e8 nessuna sveglia alle " + std::wstring(time) + L".";
    return L"Ho cancellato la sveglia delle " + std::wstring(time) + L".";
}

JsonObject^ Actions::TimerStatus() {
    auto values = ApplicationData::Current->LocalSettings->Values;
    unsigned long long end = values->HasKey(ref new String(TimerEndKey)) ? safe_cast<unsigned long long>(values->Lookup(ref new String(TimerEndKey))) : 0;
    unsigned long long now = NowTicks();
    JsonObject^ o = ref new JsonObject();
    bool running = end > now;
    o->SetNamedValue(L"timer_attivo", JsonValue::CreateBooleanValue(running));
    if (running) o->SetNamedValue(L"secondi_rimasti", JsonValue::CreateNumberValue(static_cast<double>((end - now) / 10000000ULL)));
    return o;
}

task<std::wstring> Actions::CancelTimer() {
    ApplicationData::Current->LocalSettings->Values->Remove(ref new String(TimerEndKey));
    return Launch("orologio:timer?annulla=1", L"Timer annullato.", L"Non riesco ad aprire l'Orologio.");
}
