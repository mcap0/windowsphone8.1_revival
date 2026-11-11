#include "pch.h"
#include "MainPage.xaml.h"
#include "OfflineCommand.h"
#include "Actions.h"
#include "Agenda.h"
#include "Capture.h"
#include "Logger.h"
#include "Sounds.h"
#include "Core/Audio.h"
#include "Core/Text.h"
#include "Core/Command.h"
#include "VoskEngine.h"

#include <cmath>

using namespace Assistente;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Foundation;
using namespace Windows::Media::Devices;
using namespace Windows::Media::SpeechSynthesis;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

namespace {

const wchar_t* const Examples[] = {
    L"metti un timer di 5 minuti",
    L"svegliami domani alle 7 e mezza",
    L"prendi nota: comprare il latte",
    L"metti un po' di Battiato",
    L"che tempo fa domani a Palermo?",
    L"quanto fa 18 per 23?",
    L"come si dice \"buonanotte\" in inglese?",
};
const unsigned ExampleCount = sizeof(Examples) / sizeof(Examples[0]);

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Arg(JsonObject^ args, const wchar_t* name) {
    String^ k = ref new String(name);
    if (args == nullptr || !args->HasKey(k)) return std::wstring();
    IJsonValue^ v = args->GetNamedValue(k);
    if (v->ValueType == JsonValueType::String) return v->GetString()->Data();
    if (v->ValueType == JsonValueType::Number) return std::to_wstring(static_cast<long long>(v->GetNumber()));
    return std::wstring();
}

assist::LocalTime Now() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    assist::LocalTime n = {t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wDayOfWeek};
    return n;
}

// What the recording mode asks to read (tools/wsr, VOCE-PIANO.md 5): the 7 vowels held, syllables, words, commands.
// show: on screen; say: the transcript saved with the WAV. Accented letters only as \uXXXX (MSVC reads ANSI).
struct RecPrompt {
    const wchar_t* show;
    const wchar_t* say;
};
const RecPrompt RecPrompts[] = {
    {L"a", L"a"},
    {L"\u00E8", L"\u00E8"},
    {L"\u00E9", L"\u00E9"},
    {L"i", L"i"},
    {L"\u00F2", L"\u00F2"},
    {L"\u00F3", L"\u00F3"},
    {L"u", L"u"},
    {L"pa pe pi po pu", L"pa pe pi po pu"},
    {L"ta te ti to tu", L"ta te ti to tu"},
    {L"ca che chi co cu", L"ca che chi co cu"},
    {L"ma me mi mo mu", L"ma me mi mo mu"},
    {L"la le li lo lu", L"la le li lo lu"},
    {L"sa se si so su", L"sa se si so su"},
    {L"fa fe fi fo fu", L"fa fe fi fo fu"},
    {L"ba be bi bo bu", L"ba be bi bo bu"},
    {L"da de di do du", L"da de di do du"},
    {L"na ne ni no nu", L"na ne ni no nu"},
    {L"ra re ri ro ru", L"ra re ri ro ru"},
    {L"va ve vi vo vu", L"va ve vi vo vu"},
    {L"casa, sera, bene, vita", L"casa, sera, bene, vita"},
    {L"cosa, sole, luna, mamma", L"cosa, sole, luna, mamma"},
    {L"gnocchi, sveglia, zero, chiama", L"gnocchi, sveglia, zero, chiama"},
    {L"metti un timer di 5 minuti", L"metti un timer di cinque minuti"},
    {L"svegliami domani alle 7 e mezza", L"svegliami domani alle sette e mezza"},
    {L"chiama mamma", L"chiama mamma"},
    {L"chiama casa", L"chiama casa"},
    {L"metti un po' di Battiato", L"metti un po' di Battiato"},
    {L"fammi sentire Lucio Dalla", L"fammi sentire Lucio Dalla"},
    {L"metti in pausa", L"metti in pausa"},
    {L"canzone precedente", L"canzone precedente"},
    {L"ferma la musica", L"ferma la musica"},
    {L"che tempo fa domani", L"che tempo fa domani"},
    {L"meteo Milano", L"meteo Milano"},
    {L"che ore sono", L"che ore sono"},
    {L"che giorno \u00E8 oggi", L"che giorno \u00E8 oggi"},
    {L"cosa ho domani", L"cosa ho domani"},
    {L"leggi il calendario", L"leggi il calendario"},
    {L"metti la sveglia alle 6", L"metti la sveglia alle sei"},
    {L"svegliami venerd\u00EC alle nove", L"svegliami venerd\u00EC alle nove"},
    {L"che sveglie ho impostato", L"che sveglie ho impostato"},
    {L"a che punto \u00E8 il timer", L"a che punto \u00E8 il timer"},
    {L"cancella timer", L"cancella timer"},
    {L"apri il calendario", L"apri il calendario"},
    {L"apri le impostazioni wi-fi", L"apri le impostazioni wi-fi"},
    {L"impostazioni bluetooth", L"impostazioni bluetooth"},
    {L"prendi nota: comprare il latte", L"prendi nota comprare il latte"},
    {L"ricordami alle 6 di chiamare il dentista", L"ricordami alle sei di chiamare il dentista"},
    {L"metti musica a caso", L"metti musica a caso"},
    {L"salta", L"salta"},
    {L"riprendi", L"riprendi"},
};
const size_t RecCount = sizeof(RecPrompts) / sizeof(RecPrompts[0]);

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
}  // namespace

bool MainPage::NoListenOnResume = false;



MainPage::MainPage()
    : _busy(false), _listening(false), _discard(false), _micFound(false), _lastTurn(0), _silent(false), _vosk(false), _rec(false),
      _recStop(false), _recIndex(0), _recRetries(0), _recSaved(0), _recText(false), _recLong(false), _recPieces(0) {
    InitializeComponent();
    auto values = ApplicationData::Current->LocalSettings->Values;
    unsigned example = values->HasKey(L"esempio") ? safe_cast<unsigned>(values->Lookup(L"esempio")) : 0;
    values->Insert(L"esempio", (example + 1) % ExampleCount);
    HintText->Text = ref new String((L"prova \"" + std::wstring(Examples[example % ExampleCount]) + L"\"").c_str());
}

void MainPage::OnNavigatedTo(NavigationEventArgs^ e) {
    Page::OnNavigatedTo(e);
    Logger::Start(L"http://192.168.1.148:8765/assistente");
    Sounds::Prepare();  // XAudio2 and the WAVs before the first listen: the earcon must not wait for them
    Windows::UI::ViewManagement::StatusBar::GetForCurrentView()->HideAsync();  // full screen, like the Bing voice screen
    OfflineCommand::Prepare();
    VoskEngine::Prepare();
    String^ reason = dynamic_cast<String^>(e->Parameter);
    Listen(reason != nullptr ? reason : L"avvio");
}

// The big line and the line in the box say the same thing, like the Bing voice screen.
void MainPage::SetState(const std::wstring& text) {
    StateText->Text = ref new String(text.c_str());
    BoxHint->Text = StateText->Text;
    HintPanel->Visibility = _listening ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
}

int MainPage::CountToday(const wchar_t* what, bool increment) {
    assist::LocalTime n = Now();
    wchar_t key[48];
    swprintf_s(key, L"conto.%s.%04d%02d%02d", what, n.year, n.month, n.day);
    auto values = ApplicationData::Current->LocalSettings->Values;
    int count = values->HasKey(ref new String(key)) ? safe_cast<int>(values->Lookup(ref new String(key))) : 0;
    if (increment) values->Insert(ref new String(key), ++count);
    return count;
}

// The Lumia 1020 records only from the "Enhanced Audio Recording Device" (apps/Registratore found it);
// a plugged-in headset appears as "Headset Microphone" and wins, a Bluetooth one (not from the QCAUD driver)
// wins over both. Looked up at every listen (plug/unplug).
void MainPage::FindMicrophone(std::function<void()> then) {
    // the microphone found last time is used at once (the search takes ~0.4 s); the search runs again for the next time
    auto settings = Windows::Storage::ApplicationData::Current->LocalSettings->Values;
    if (!_micFound && settings->HasKey(L"microfono")) {  // the one of the last run (LocalSettings)
        _micId = safe_cast<String^>(settings->Lookup(L"microfono"))->Data();
        _micFound = true;
    }
    if (_micFound) {
        then();
        then = nullptr;
    }
    create_task(DeviceInformation::FindAllAsync(MediaDevice::GetAudioCaptureSelector())).then([this, then](task<DeviceInformationCollection^> t) {
        std::wstring enhanced, headset, bluetooth;
        try {
            for (auto d : t.get()) {
                if (!d->IsEnabled) continue;
                std::wstring name = d->Name->Data();
                if (name.find(L"QCAUD") == std::wstring::npos || name.find(L"Bluetooth") != std::wstring::npos ||
                    name.find(L"Hands-Free") != std::wstring::npos) bluetooth = d->Id->Data();
                else if (name.compare(0, 18, L"Headset Microphone") == 0) headset = d->Id->Data();
                else if (name.find(L"Enhanced") != std::wstring::npos) enhanced = d->Id->Data();
            }
        } catch (Exception^) {
        }
        _micId = !bluetooth.empty() ? bluetooth : headset.empty() ? enhanced : headset;
        if (!_micFound || then) Logger::Write(L"microfono: " + (_micId.empty() ? std::wstring(L"predefinito") : _micId));
        _micFound = true;
        Windows::Storage::ApplicationData::Current->LocalSettings->Values->Insert(L"microfono", ref new String(_micId.c_str()));
        if (then) then();
    }, task_continuation_context::use_current());
}

void MainPage::Listen(String^ reason) {
    // assistente:chiedi?q=...: a typed question (tests from the PC through Esplora "open")
    std::wstring r = reason != nullptr ? reason->Data() : L"";
    if (r.compare(0, 18, L"assistente:chiedi?") == 0) {
        if (_busy) return;
        std::wstring typed;
        try {
            typed = (ref new Uri(reason))->QueryParsed->GetFirstValueByName(L"q")->Data();
        } catch (Exception^) {}
        if (typed.empty()) return;
        _busy = true;
        Player->Stop();
        ExtrasPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        AnswerText->Text = ref new String(typed.c_str());
        DetailText->Text = L"";
        SetState(L"Penso...");
        Logger::Write(L"chiesto: " + typed);
        TryOffline(std::wstring(), std::string(), typed);
        return;
    }
    if (r.compare(0, 19, L"assistente:registra") == 0) {  // from the PC (Esplora "open"): assistente:registra?chi=nome
        if (_busy) return;
        std::wstring who;
        try {
            who = (ref new Uri(reason))->QueryParsed->GetFirstValueByName(L"chi")->Data();
        } catch (Exception^) {}
        StartRecording(who);
        return;
    }
    if (_busy || _rec) return;
    _busy = true;
    _listening = true;
    _discard = false;
    Player->Stop();
    AnswerText->Text = L"";
    DetailText->Text = L"";
    ChoicesPanel->Children->Clear();
    ExtrasPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    SetState(L"Ti ascolto...");
    Logger::Write(L"ascolto (" + std::wstring(reason->Data()) + L")");
    FindMicrophone([this]() {
        // The earcon and then the microphone: while the capture is running the system holds the app's playback back,
        // so a beep played together with the microphone is not heard (and comes out at the end, with the answer).
        // Pre-initialized sounds (Sounds::Prepare) make the 166 ms beep start here; the microphone opens 220 ms later.
        auto listen = [this]() {
            Sounds::Play(Sounds::Listening);
            auto wait = ref new DispatcherTimer();
            TimeSpan t;
            t.Duration = 2200000LL;
            wait->Interval = t;
            wait->Tick += ref new EventHandler<Object^>([this, wait](Object^, Object^) {
                wait->Stop();
                if (_discard) OnHeard(std::vector<int16_t>(), false, std::wstring());  // went to type meanwhile
                else StartCapture();
            });
            wait->Start();
        };
        if (VoskEngine::Coming()) {  // Vosk queues the audio if the model is still loading
            listen();
            return;
        }
        WhenVoskReady(listen);  // no Vosk: the phone's own recognizer first
    });
}

// Vosk ready (or failed: then the phone's own recognizer is used): every 300 ms, up to 30 s, "Preparo la voce..."
void MainPage::WhenVoskReady(std::function<void()> then) {
    if (VoskEngine::Coming() || VoskEngine::State().compare(0, 6, L"pronto") == 0 ||
        VoskEngine::State().find(L"fallit") != std::wstring::npos || VoskEngine::State().find(L"spento") != std::wstring::npos ||
        VoskEngine::State().find(L"non ") != std::wstring::npos) {
        OfflineCommand::WhenReady(1500, then);
        return;
    }
    SetState(L"Preparo la voce...");
    DetailText->Text = ref new String(VoskEngine::State().c_str());
    auto t = ref new DispatcherTimer();
    TimeSpan step;
    step.Duration = 3000000LL;
    t->Interval = step;
    auto start = std::make_shared<ULONGLONG>(GetTickCount64());
    t->Tick += ref new EventHandler<Object^>([this, t, then, start](Object^, Object^) {
        DetailText->Text = ref new String(VoskEngine::State().c_str());
        if (VoskEngine::Ready() || GetTickCount64() - *start > 30000) {
            t->Stop();
            DetailText->Text = L"";
            SetState(L"Ti ascolto...");
            then();
        }
    });
    t->Start();
}

// Test builds: the page as rendered, to compare its pixels with the original Bing screen (share/inbox/assistente-shot.txt).
void MainPage::SendShot() {
    auto rtb = ref new Windows::UI::Xaml::Media::Imaging::RenderTargetBitmap();
    create_task(rtb->RenderAsync(this))
        .then([rtb]() { return rtb->GetPixelsAsync(); })
        .then([rtb](IBuffer^ pixels) {
            Array<unsigned char>^ bgra = nullptr;
            Windows::Security::Cryptography::CryptographicBuffer::CopyToByteArray(pixels, &bgra);
            auto stream = ref new InMemoryRandomAccessStream();
            unsigned w = rtb->PixelWidth, h = rtb->PixelHeight;
            return create_task(Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
                                   Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId, stream))
                .then([bgra, w, h](Windows::Graphics::Imaging::BitmapEncoder^ enc) {
                    enc->SetPixelData(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                                      Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied, w, h, 96, 96, bgra);
                    return enc->FlushAsync();
                })
                .then([stream]() {
                    auto reader = ref new DataReader(stream->GetInputStreamAt(0));
                    return create_task(reader->LoadAsync(static_cast<unsigned int>(stream->Size))).then([reader](unsigned int n) {
                        return Windows::Security::Cryptography::CryptographicBuffer::EncodeToBase64String(reader->ReadBuffer(n));
                    });
                });
        })
        .then([](task<String^> t) {
            try {
                Logger::PostNow(L"assistente-shot", t.get());
            } catch (Exception^ ex) {
                Logger::Write(L"schermata: " + std::wstring(ex->Message->Data()));
            }
        });
}

void MainPage::StartCapture() {
    _vosk = VoskEngine::Coming();
    if (_vosk && !VoskEngine::Ready()) DetailText->Text = L"Carico la voce: parla pure, le parole arrivano tra poco.";
    if (_vosk) {
        // Vosk: the words appear while speaking; the final text goes to the command network as if typed
        VoskEngine::Begin([this](const std::wstring& text, bool final) {
            if (!final) {
                if (_listening && !text.empty()) {
                    // the words right under "Ti ascolto...": the examples make room
                    HintPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                    DetailText->Text = L"";
                    AnswerText->Text = ref new String(text.c_str());
                }
                return;
            }
            Logger::Write(L"sentito (vosk): " + text);
            // Vosk's endpoint ends the utterance before the capture has stopped: listening is over now, or Speak
            // (which keeps quiet while listening) would never say the answer (1.5.19-1.5.20: no voice at all)
            _listening = false;
            Meter->Width = 0;
            if (_discard) return;
            DetailText->Text = L"";
            if (text.empty()) {
                _busy = false;
                SetState(L"Non ho capito");
                DetailText->Text = L"Premi la freccia e riprova.";
                return;
            }
            AnswerText->Text = ref new String(text.c_str());
            TryOffline(text, std::string(), std::wstring());
        });
        Capture::SetStream([](const std::vector<int16_t>& pcm) { VoskEngine::Feed(pcm); }, true);
    } else {
        Capture::SetStream(nullptr);
    }
    if (!_vosk) {  // no Vosk: the phone's recognizer has the microphone to itself
        OfflineCommand::ListenAlone([this](const std::wstring& text) {
            _listening = false;
            Meter->Width = 0;
            if (_discard) {
                _busy = false;
                    SetState(L"Scrivi la domanda");
                return;
            }
            SetState(L"Penso...");
            TryOffline(text, std::string(), std::wstring());
        });
        return;
    }
    auto shot = ref new DispatcherTimer();
    TimeSpan later;
    later.Duration = 15000000LL;  // 1.5 s into listening
    shot->Interval = later;
    shot->Tick += ref new EventHandler<Object^>([this, shot](Object^, Object^) {
        shot->Stop();
        SendShot();
    });
    Capture::Start(_micId,
        [this](double level) {
            Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, level]() {
                double w = Root->ActualWidth;
                Meter->Width = std::max(0.0, std::min(w, w * level));
            }));
        },
        [this](const std::vector<int16_t>& pcm, bool heard, const std::wstring& error) {
            Logger::Write(L"cattura finita: " + std::to_wstring(pcm.size()) + L" campioni");
            std::vector<int16_t> copy = pcm;
            std::wstring err = error;
            Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy, heard, err]() {
                OnHeard(copy, heard, err);
            }));
        });
}

void MainPage::Root_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^) {
    if (_rec) {  // recording mode: a tap ends the sentence now (it is saved); the long mode ends only by the arrow
        if (_listening && !_recLong) Capture::StopNow();
        return;
    }
    if (_listening && !_vosk) OfflineCommand::CancelAlone();
    else if (_listening) Capture::StopNow();
}

void MainPage::OnHeard(const std::vector<int16_t>& pcm, bool heard, const std::wstring& error) {
    _listening = false;
    Meter->Width = 0;
    if (_vosk && (_discard || !error.empty() || !heard)) VoskEngine::End();
    if (_discard) {  // the user went to the box to type
        _busy = false;
            SetState(L"Scrivi la domanda");
        return;
    }
    Logger::Write(L"audio: " + std::to_wstring(pcm.size() / 16) + L" ms, " + Capture::LastFormat() +
                  (error.empty() ? L"" : L", errore " + error));
    if (!error.empty()) {
        SetState(L"Microfono non disponibile");
        DetailText->Text = ref new String(error.c_str());
        _busy = false;
        return;
    }
    if (!heard) {
        SetState(L"Non ho sentito niente");
        DetailText->Text = L"Premi \"parla\" e riprova.";
        _busy = false;
        return;
    }
    SetState(L"Penso...");
    if (_vosk) {  // the final text comes from VoskEngine (StartCapture's callback)
        VoskEngine::End();
        return;
    }
    std::string wav = assist::WavBytes(pcm, 16000);
    OfflineCommand::Finish([this, wav](const std::wstring& text) { TryOffline(text, wav, std::wstring()); });
}

namespace {
// functions that read data: the answer is phrased from it (Phrase)
bool IsInfo(const std::wstring& name) {
    return name == L"leggi_calendario" || name == L"elenca_sveglie" || name == L"stato_timer";
}

}  // namespace

namespace {
std::wstring Minutes(int seconds) {
    int m = seconds / 60, s = seconds % 60;
    std::wstring t;
    if (m > 0) t = m == 1 ? L"un minuto" : std::to_wstring(m) + L" minuti";
    if (s > 0) t += (t.empty() ? L"" : L" e ") + (s == 1 ? std::wstring(L"un secondo") : std::to_wstring(s) + L" secondi");
    return t.empty() ? L"meno di un secondo" : t;
}

// "2026-11-04 09:30" -> "alle 9:30"
std::wstring At(const std::wstring& start) {
    if (start.size() < 16) return std::wstring();
    int h = _wtoi(start.substr(11, 2).c_str());
    return (h == 1 ? L"all'" : L"alle ") + std::to_wstring(h) + L":" + start.substr(14, 2);
}

// the data of an info function said in Italian
std::wstring Phrase(const std::wstring& name, JsonObject^ args, JsonObject^ data, const assist::LocalTime& now) {
    if (data->HasKey(L"errore")) return data->GetNamedString(L"errore")->Data();
    if (name == L"stato_timer") {
        if (!data->GetNamedBoolean(L"timer_attivo", false)) return L"Nessun timer in corso.";
        int left = static_cast<int>(data->GetNamedNumber(L"secondi_rimasti", 0));
        return (left < 120 && left >= 60 ? L"Manca " : L"Mancano ") + Minutes(left) + L".";
    }
    if (name == L"elenca_sveglie") {
        std::vector<std::wstring> on;
        JsonArray^ list = data->GetNamedArray(L"sveglie", ref new JsonArray());
        for (unsigned i = 0; i < list->Size; ++i) {
            JsonObject^ a = list->GetObjectAt(i);
            if (!a->GetNamedBoolean(L"attiva", false)) continue;
            std::wstring at = a->GetNamedString(L"ora", L"")->Data();
            if (at.size() == 5 && at[0] == L'0') at = at.substr(1);
            std::wstring label = a->GetNamedString(L"nome", L"")->Data();
            on.push_back((at == L"1:00" || at.compare(0, 2, L"1:") == 0 ? L"all'" : L"alle ") + at +
                         (label.empty() ? L"" : L" (" + label + L")"));
        }
        if (on.empty()) return list->Size == 0 ? L"Non hai sveglie." : L"Nessuna sveglia attiva.";
        if (on.size() == 1) return L"Hai una sveglia " + on[0] + L".";
        std::wstring t = L"Hai " + std::to_wstring(on.size()) + L" sveglie: ";
        for (size_t i = 0; i < on.size(); ++i) t += (i == 0 ? L"" : i + 1 == on.size() ? L" e " : L", ") + on[i];
        return t + L".";
    }
    // leggi_calendario
    std::wstring from = args->GetNamedString(L"dal", L"")->Data();
    int days = _wtoi(args->GetNamedString(L"giorni", L"1")->Data());
    int y = 0, m = 0, d = 0;
    swscanf_s(from.c_str(), L"%d-%d-%d", &y, &m, &d);
    long long offset = assist::DaysFromCivil(y, m, d) - assist::DaysFromCivil(now.year, now.month, now.day);
    std::wstring when = days > 1 ? L"Nei prossimi " + std::to_wstring(days) + L" giorni" :
                        offset == 0 ? L"Oggi" : offset == 1 ? L"Domani" : Wide(assist::ItalianDate(y, m, d));
    if (days == 1 && offset > 1) when[0] = towupper(when[0]);
    JsonArray^ events = data->GetNamedArray(L"eventi", ref new JsonArray());
    if (events->Size == 0) return when + L" non hai impegni.";
    std::wstring t = when + (events->Size == 1 ? L" hai un impegno: " : L" hai " + std::to_wstring(events->Size) + L" impegni: ");
    unsigned shown = std::min(events->Size, 6u);
    for (unsigned i = 0; i < shown; ++i) {
        JsonObject^ e = events->GetObjectAt(i);
        std::wstring start = e->GetNamedString(L"inizio", L"")->Data(), item;
        if (days > 1 && start.size() >= 10) {
            int ey = _wtoi(start.substr(0, 4).c_str()), em = _wtoi(start.substr(5, 2).c_str()), ed = _wtoi(start.substr(8, 2).c_str());
            item = Wide(assist::ItalianDate(ey, em, ed)) + L" ";
        }
        if (!e->GetNamedBoolean(L"tutto_il_giorno", false)) item += At(start) + L" ";
        item += e->GetNamedString(L"titolo", L"")->Data();
        t += (i ? L"; " : L"") + item;
    }
    if (events->Size > shown) t += L"; e altri " + std::to_wstring(events->Size - shown);
    return t + L".";
}

std::vector<std::string> OpenTargetsUtf8() {
    std::vector<std::string> out;
    std::wstring all = Actions::OpenTargets();
    size_t start = 0;
    while (start < all.size()) {
        size_t comma = all.find(L", ", start);
        if (comma == std::wstring::npos) comma = all.size();
        out.push_back(Utf8(all.substr(start, comma - start)));
        start = comma + 2;
    }
    return out;
}
}  // namespace

// What the recognizer heard (or what was typed) through the hand-written network: on the phone when it is sure,
// otherwise a Google search with the sentence.
void MainPage::TryOffline(const std::wstring& heard, const std::string& wav, const std::wstring& typed) {
    std::wstring text = heard.empty() ? typed : heard;
    const assist::Wsp& net = OfflineCommand::Net();
    std::wstring why = !net.Loaded() ? L"rete non caricata" : text.empty() ? L"" : L"";
    if (!text.empty() && net.Loaded()) {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        assist::WspResult r;
        assist::Command cmd;
        static std::vector<std::string> targets = OpenTargetsUtf8();
        bool ok = assist::Understand(net, Utf8(text), Now(), targets, cmd, r);  // whole text, then its parts
        QueryPerformanceCounter(&b);
        long long us = (b.QuadPart - a.QuadPart) * 1000000LL / f.QuadPart;
        std::wstring args;
        for (const auto& kv : cmd.args) args += L" " + Wide(kv.first) + L"=\"" + Wide(kv.second) + L"\"";
        wchar_t margin[16];
        swprintf_s(margin, L"%.2f", r.margin);
        Logger::Write(L"wsp: \"" + text + L"\" -> " + Wide(r.best) + L" " + margin + L" " +
                      (ok ? L"telefono" + args : L"ricerca (" + Wide(cmd.why) + L")") + L", " + std::to_wstring(us) + L" us");
        if (ok) {
            RunOffline(cmd, text, L"\u201c" + text + L"\u201d \u2014 sul telefono (" + std::to_wstring(us) + L" \u00b5s)");
            return;
        }
        why = L"la rete dice " + Wide(r.best) + L" " + margin + L" (" + Wide(cmd.why) + L")";
    }
    std::wstring detail = heard.empty() && typed.empty() ? L"riconoscitore: " + OfflineCommand::Status()
                                                          : L"\u201c" + text + L"\u201d: " + why;
    // not a command: a question for Google, without the openings and the politeness
    std::wstring query = net.Loaded() && !text.empty() ? Wide(assist::SearchQuery(net, Utf8(text))) : std::wstring();
    if (!query.empty()) {
        Logger::Write(L"ricerca: \"" + query + L"\" (" + detail + L")");
        CountToday(L"ricerca", true);
        assist::Command search;
        search.name = "cerca_sul_web";
        search.args.push_back(std::make_pair(std::string("domanda"), Utf8(query)));
        RunOffline(search, text, detail);
        return;
    }
    Logger::Write(L"non capito: " + detail);
    _silent = false;
    Answer(L"Non ho capito.", L"Non ho capito", detail);
    return;
}

void MainPage::RunOffline(const assist::Command& cmd, const std::wstring& text, const std::wstring& detail) {
    CountToday(L"offline", true);
    auto finish = [this, detail](const std::wstring& said) {
        std::wstring say = said.empty() && !_silent ? std::wstring(L"Fatto.") : said;
        _lastTurn = GetTickCount64();
        Answer(say, L"Fatto", detail);
    };
    if (!cmd.say.empty()) {
        finish(Wide(cmd.say));
        return;
    }
    ToolCall call;
    call.name = Wide(cmd.name);
    call.id = nullptr;
    call.args = ref new JsonObject();
    for (const auto& kv : cmd.args)
        call.args->SetNamedValue(ref new String(Wide(kv.first).c_str()), JsonValue::CreateStringValue(ref new String(Wide(kv.second).c_str())));
    if (IsInfo(call.name)) {
        assist::LocalTime now = Now();
        RunInfo(call).then([this, call, finish, now](task<JsonObject^> t) {
            std::wstring say;
            try { say = Phrase(call.name, call.args, t.get(), now); }
            catch (Exception^ ex) { say = ex->Message->Data(); }
            Logger::Write(L"risultato " + call.name + L": " + say);
            finish(say);
        }, task_continuation_context::use_current());
        return;
    }
    RunCall(call).then([finish](task<std::wstring> t) {
        std::wstring say;
        try { say = t.get(); }
        catch (Exception^ ex) { say = ex->Message->Data(); }
        finish(say);
    }, task_continuation_context::use_current());
}

task<std::wstring> MainPage::RunCall(const ToolCall& call) {
    std::wstring n = call.name;
    JsonObject^ a = call.args;
    Logger::Write(L"funzione " + n + L" " + std::wstring(a->Stringify()->Data()));
    if (n == L"imposta_timer") {
        int seconds = _wtoi(Arg(a, L"secondi").c_str());
        return Actions::Timer(seconds, Arg(a, L"etichetta"));
    }
    if (n == L"imposta_sveglia") {
        int h = 0, m = 0;
        assist::LocalTime when;
        if (!assist::ParseHourMinute(Utf8(Arg(a, L"ora")), h, m) ||
            !assist::AlarmMoment(Now(), h, m, Utf8(Arg(a, L"giorno")), when))
            return task_from_result(std::wstring(L"Non ho capito l'ora della sveglia."));
        std::wstring repeat = Arg(a, L"ripeti");
        int days = repeat == L"ogni giorno" ? 0x7f : repeat == L"feriali" ? 0x1f : repeat == L"weekend" ? 0x60 : 0;
        return Actions::Alarm(when.year, when.month, when.day, when.hour, when.minute, days, Arg(a, L"etichetta"));
    }
    if (n == L"promemoria") {
        int h = 0, m = 0;
        assist::LocalTime when;
        if (!assist::ParseHourMinute(Utf8(Arg(a, L"ora")), h, m) ||
            !assist::AlarmMoment(Now(), h, m, Utf8(Arg(a, L"giorno")), when))
            return task_from_result(std::wstring(L"Non ho capito quando ricordartelo."));
        return task_from_result(Actions::Reminder(Arg(a, L"testo"), when));
    }
    if (n == L"prendi_nota") {
        std::wstring text = Arg(a, L"testo"), title = Arg(a, L"titolo");
        if (title.empty()) title = L"Nota vocale";
        return task_from_result(Actions::Note(title, text));
    }
    if (n == L"riproduci_musica") return Actions::Music(Arg(a, L"richiesta"), Arg(a, L"tipo"), Arg(a, L"casuale") == L"si");
    if (n == L"controlla_musica") return Actions::MusicControl(Arg(a, L"azione"));
    if (n == L"cerca_sul_web") return Actions::WebSearch(Arg(a, L"domanda"));
    if (n == L"chiama") return Actions::Call(Arg(a, L"chi"));
    if (n == L"meteo") {  // the Meteo app shows it: no voice, no text
        _silent = true;
        return Actions::Weather(Arg(a, L"luogo"), Arg(a, L"giorno"));
    }
    if (n == L"apri") return Actions::Open(Arg(a, L"cosa"));
    if (n == L"aggiungi_evento")
        return Agenda::Add(Arg(a, L"titolo"), Arg(a, L"giorno"), Arg(a, L"ora"), _wtoi(Arg(a, L"durata_minuti").c_str()),
                           Arg(a, L"luogo"));
    if (n == L"cancella_sveglia") {
        int h = 0, m = 0;
        if (!assist::ParseHourMinute(Utf8(Arg(a, L"ora")), h, m))
            return task_from_result(std::wstring(L"Non ho capito quale sveglia."));
        return task_from_result(Actions::RemoveAlarm(h, m));
    }
    if (n == L"annulla_timer") return Actions::CancelTimer();
    return task_from_result(std::wstring(L"Non so ancora fare: " + n));
}

task<JsonObject^> MainPage::RunInfo(const ToolCall& call) {
    Logger::Write(L"funzione " + call.name + L" " + std::wstring(call.args->Stringify()->Data()));
    if (call.name == L"leggi_calendario") {
        int days = _wtoi(Arg(call.args, L"giorni").c_str());
        return Agenda::Read(Arg(call.args, L"dal"), days > 0 ? days : 1);
    }
    if (call.name == L"elenca_sveglie") return task_from_result(Actions::ListAlarms());
    return task_from_result(Actions::TimerStatus());
}

void MainPage::Answer(const std::wstring& say, const wchar_t* state, const std::wstring& detail) {
    if (_silent) {  // an app took over (meteo): nothing to say
        _silent = false;
        _busy = false;
        SetState(L"Fatto");
        AnswerText->Text = L"";
        DetailText->Text = ref new String(detail.c_str());
        return;
    }
    SetState(state);
    AnswerText->Text = ref new String(say.c_str());
    DetailText->Text = ref new String(detail.c_str());
    _busy = false;
    Logger::Write(L"mostro: " + say);
    ShowCallChoices();
    std::wstring number, name;
    if (Actions::TakePendingCall(number, name)) {
        // no voice: it would talk over the call screen, which already shows who is being called
        Logger::Write(L"chiamata: " + name + L" " + number);
        NoListenOnResume = true;
        Windows::ApplicationModel::Calls::PhoneCallManager::ShowPhoneCallUI(ref new String(number.c_str()),
                                                                           ref new String(name.c_str()));
        return;
    }
    Sounds::Play(Sounds::Results);
    Speak(say);
}

// The contacts that sound like the name heard: one row per number, a tap opens the call screen.
void MainPage::ShowCallChoices() {
    using namespace Windows::UI::Xaml;
    using namespace Windows::UI::Xaml::Controls;
    using namespace Windows::UI::Xaml::Media;
    ChoicesPanel->Children->Clear();
    for (const auto& c : Actions::TakeCallChoices()) {
        auto row = ref new StackPanel();
        row->Margin = Thickness(0, 10, 0, 10);
        row->Background = ref new SolidColorBrush(Windows::UI::Colors::Transparent);  // the whole row takes the tap
        auto title = ref new TextBlock();
        title->Text = ref new String(c.name.c_str());
        title->FontSize = 30;
        title->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
        title->TextWrapping = TextWrapping::Wrap;
        auto sub = ref new TextBlock();
        sub->Text = ref new String((c.kind + L"  " + c.number).c_str());
        sub->FontSize = 20;
        sub->Foreground = safe_cast<Brush^>(Application::Current->Resources->Lookup(L"PhoneAccentBrush"));
        row->Children->Append(title);
        row->Children->Append(sub);
        std::wstring number = c.number, name = c.name;
        row->Tapped += ref new Input::TappedEventHandler([this, number, name](Object^, Input::TappedRoutedEventArgs^ e) {
            e->Handled = true;  // not the page's tap-to-listen
            Logger::Write(L"chiamata scelta: " + name + L" " + number);
            ChoicesPanel->Children->Clear();
            NoListenOnResume = true;
            Windows::ApplicationModel::Calls::PhoneCallManager::ShowPhoneCallUI(ref new String(number.c_str()),
                                                                               ref new String(name.c_str()));
        });
        ChoicesPanel->Children->Append(row);
    }
}

// Something threw on the UI thread: say it on screen and in the log instead of letting the app die.
void MainPage::Fail(const wchar_t* where, Exception^ ex) {
    std::wstring msg = std::wstring(where) + L": " + (ex->Message != nullptr ? ex->Message->Data() : L"?") + L" " +
                       std::to_wstring(static_cast<unsigned>(ex->HResult));
    Logger::Write(L"errore " + msg);
    _busy = false;
    _listening = false;
    SetState(L"Problema");
    AnswerText->Text = ref new String(msg.c_str());
}

// system voice, Italian (Elsa, else any it-IT voice installed from upgrade/it-it)
void MainPage::Speak(const std::wstring& text) {
    if (text.empty() || _listening) return;
    if (_synth == nullptr) {
        _synth = ref new SpeechSynthesizer();
        VoiceInformation^ pick = nullptr;
        for (VoiceInformation^ v : SpeechSynthesizer::AllVoices) {
            if (_wcsicmp(v->Language->Data(), L"it-IT") != 0) continue;
            if (pick == nullptr || std::wstring(v->DisplayName->Data()).find(L"Elsa") != std::wstring::npos) pick = v;
        }
        if (pick != nullptr) _synth->Voice = pick;
        Logger::Write(L"voce: " + std::wstring(pick != nullptr ? pick->DisplayName->Data() : L"predefinita (niente it-IT)"));
    }
    create_task(_synth->SynthesizeTextToStreamAsync(ref new String(text.c_str())))
        .then([this](task<SpeechSynthesisStream^> t) {
            try {
                auto s = t.get();
                if (_listening) return;  // already listening again: the old answer would talk over the user
                Player->SetSource(s, s->ContentType);
                Player->Play();
            } catch (Exception^ ex) {
                Logger::Write(L"voce: " + std::wstring(ex->Message->Data()));
            }
        }, task_continuation_context::use_current());
}

// The arrow: sends what was typed; while listening it ends the sentence now; otherwise listens again.
void MainPage::Send_Click(Object^, RoutedEventArgs^) {
    std::wstring typed = Box->Text->Data();
    if (_rec) {  // recording mode: the arrow ends it
        _recStop = true;
        if (_listening) Capture::StopNow();
        else StopRecordingMode(L"fermato");
        return;
    }
    if (typed.compare(0, 8, L"registra") == 0 || typed.compare(0, 8, L"Registra") == 0) {
        if (_listening) {
            _discard = true;
            { if (_vosk) Capture::StopNow(); else OfflineCommand::CancelAlone(); }
        }
        if (_busy && !_listening) return;
        Box->Text = L"";
        Focus(Windows::UI::Xaml::FocusState::Programmatic);
        std::wstring who = typed.size() > 9 ? typed.substr(9) : std::wstring();
        if (who.compare(0, 5, L"lungo") == 0) StartLongRecording(who.size() > 6 ? who.substr(6) : std::wstring());
        else if (who.compare(0, 5, L"testo") == 0) StartTextRecording(who.size() > 6 ? who.substr(6) : std::wstring(), L"testo");
        else if (who.compare(0, 7, L"comandi") == 0) StartTextRecording(who.size() > 8 ? who.substr(8) : std::wstring(), L"comandi");
        else if (who.compare(0, 5, L"prova") == 0) StartTextRecording(who.size() > 6 ? who.substr(6) : std::wstring(L"speaker"), L"prova");
        else StartRecording(who);
        return;
    }
    if (!typed.empty()) {
        if (_listening) {
            _discard = true;
            { if (_vosk) Capture::StopNow(); else OfflineCommand::CancelAlone(); }
        }
        if (_busy && !_listening) return;
        _busy = true;
        _listening = false;
        Box->Text = L"";
        Player->Stop();
        ExtrasPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        AnswerText->Text = ref new String(typed.c_str());
        DetailText->Text = L"";
        Focus(Windows::UI::Xaml::FocusState::Programmatic);  // closes the keyboard
        SetState(L"Penso...");
        Logger::Write(L"scritto: " + typed);
        TryOffline(std::wstring(), std::string(), typed);
        return;
    }
    if (_listening) { if (_vosk) Capture::StopNow(); else OfflineCommand::CancelAlone(); }
    else Listen(L"freccia");
}

void MainPage::Send_Tapped(Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e) {
    e->Handled = true;
    Send_Click(sender, nullptr);
}

// The press feel of the system buttons: the tilt of PointerDown/PointerUpThemeAnimation.
void MainPage::Send_PointerPressed(Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^) {
    Tilt(true);
}

void MainPage::Send_PointerReleased(Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^) {
    Tilt(false);
}

void MainPage::Tilt(bool down) {
    using namespace Windows::UI::Xaml::Media::Animation;
    auto board = ref new Storyboard();
    Timeline^ t = down ? static_cast<Timeline^>(ref new PointerDownThemeAnimation())
                       : static_cast<Timeline^>(ref new PointerUpThemeAnimation());
    Storyboard::SetTarget(t, Arrow);
    board->Children->Append(t);
    board->Begin();
}

void MainPage::Box_KeyDown(Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e) {
    if (e->Key == VirtualKey::Enter) {
        e->Handled = true;
        Send_Click(sender, nullptr);
    }
}

void MainPage::Box_GotFocus(Object^, RoutedEventArgs^) {
    Box->Opacity = 1;
    BoxHint->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (_listening) {
        _discard = true;
        { if (_vosk) Capture::StopNow(); else OfflineCommand::CancelAlone(); }
    }
}

void MainPage::Box_LostFocus(Object^, RoutedEventArgs^) {
    Box->Opacity = 0;
    if (Box->Text->Length() == 0) BoxHint->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::Box_TextChanged(Object^, TextChangedEventArgs^) {
    if (Box->Text->Length() > 0) BoxHint->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::Examples_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e) {
    e->Handled = true;
    std::wstring t;
    for (unsigned i = 0; i < ExampleCount; ++i) t += L"\u2022 " + std::wstring(Examples[i]) + L"\n";
    AnswerText->Text = ref new String(t.c_str());
    ExtrasPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::Info_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^) {
    std::wstring t = L"Oggi: " + std::to_wstring(CountToday(L"offline", false) - CountToday(L"ricerca", false)) +
              L" comandi sul telefono, " + std::to_wstring(CountToday(L"ricerca", false)) + L" ricerche su Google.";
    DetailText->Text = ref new String(t.c_str());
}

// ---------------------------------------------------------------------------------------------- recording mode
// Each prompt: earcon, our WASAPI capture (the same path the recognizer will hear), end by the voice detector or a
// tap, then the WAV goes to LocalFolder\voce\<session>\NNN.wav (+ elenco.tsv) and to the PC inbox as base64
// (share/inbox/voce.txt; tools/wsr/inbox2wav.py turns it into files). The arrow ends the mode.

void MainPage::StartRecording(const std::wstring& speaker) {
    _busy = true;
    _rec = true;
    _recStop = false;
    _recIndex = 0;
    _recRetries = 0;
    _recSaved = 0;
    assist::LocalTime n = Now();
    wchar_t b[32];
    swprintf_s(b, L"%04d%02d%02d-%02d%02d%02d", n.year, n.month, n.day, n.hour, n.minute, n.second);
    _recSession = b;
    std::wstring who;
    for (wchar_t c : speaker) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) who += c;
    }
    if (!who.empty()) _recSession += L"-" + who;
    Player->Stop();
    ChoicesPanel->Children->Clear();
    ExtrasPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Logger::Write(L"registra: sessione " + _recSession);
    if (_keepOn == nullptr) _keepOn = ref new Windows::System::Display::DisplayRequest();
    _keepOn->RequestActive();
    _recLong = speaker.compare(0, 5, L"lungo") == 0;
    _recPieces = 0;
    if (!_recText) {
        _recList.clear();
        for (size_t i = 0; i < RecCount; ++i) _recList.push_back(std::make_pair(RecPrompts[i].show, RecPrompts[i].say));
    }
    if (!_recLong) {
        FindMicrophone([this]() { RecordNext(); });
        return;
    }
    FindMicrophone([this]() {
        _listening = true;
        SetState(L"Registro");
        HintPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        AnswerText->Text = L"Registrazione continua";
        DetailText->Text = L"la freccia per finire";
        Capture::StartLong(_micId, 30 * 16000,
            [this](double level) {
                Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, level]() {
                    double w = Root->ActualWidth;
                    Meter->Width = std::max(0.0, std::min(w, w * level));
                }));
            },
            [this](const std::vector<int16_t>& pcm) {
                std::vector<int16_t> copy = pcm;
                Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy]() {
                    wchar_t name[16];
                    swprintf_s(name, L"%03d.wav", ++_recPieces);
                    SaveTake(name, L"lungo", copy);
                    AnswerText->Text = ref new String((L"Registrazione continua: " + std::to_wstring(_recPieces * 30) + L" s").c_str());
                }));
            },
            [this](const std::vector<int16_t>& pcm, bool heard, const std::wstring& error) {
                std::vector<int16_t> copy = pcm;
                std::wstring err = error;
                Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy, heard, err]() {
                    if (heard) {
                        wchar_t name[16];
                        swprintf_s(name, L"%03d.wav", ++_recPieces);
                        SaveTake(name, L"lungo", copy);
                    }
                    StopRecordingMode(err.empty() ? std::wstring(L"fine") : L"microfono: " + err);
                }));
            });
    });
}

void MainPage::StopRecordingMode(const std::wstring& why) {
    if (_keepOn != nullptr) {
        try { _keepOn->RequestRelease(); } catch (Exception^) {}
    }
    _rec = false;
    _recLong = false;
    _recText = false;
    _listening = false;
    _busy = false;
    Meter->Width = 0;
    SetState(L"Registrazioni finite");
    AnswerText->Text = ref new String((std::to_wstring(_recSaved) + L" frasi salvate (" + why + L").").c_str());
    DetailText->Text = ref new String((L"cartella voce\\" + _recSession).c_str());
    Logger::Write(L"registra: fine (" + why + L"), " + std::to_wstring(_recSaved) + L" salvate");
    Logger::FlushNow();
}

void MainPage::RecordNext() {
    if (_recStop) {
        StopRecordingMode(L"fermato");
        return;
    }
    if (_recIndex >= _recList.size()) {
        StopRecordingMode(L"tutte");
        return;
    }
    _listening = true;
    SetState(L"Leggi");
    HintPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    AnswerText->Text = ref new String(_recList[_recIndex].first.c_str());
    std::wstring help = !_recText && _recIndex < 7 ? L"solo la vocale, tenuta un secondo (\u00E8 \u00F2 aperte come in b\u00E8ne, c\u00F2sa; "
                                        L"\u00E9 \u00F3 chiuse come in s\u00E9ra, s\u00F3le)\n" : L"";
    DetailText->Text = ref new String((help + std::to_wstring(_recIndex + 1) + L" di " + std::to_wstring(_recList.size()) +
                                       L"  \u00B7  tocca lo schermo quando hai finito, la freccia per smettere").c_str());
    Sounds::Play(Sounds::Listening);
    auto wait = ref new DispatcherTimer();
    TimeSpan t;
    t.Duration = 2200000LL;  // after the earcon's 165 ms, so the microphone does not hear it
    wait->Interval = t;
    wait->Tick += ref new EventHandler<Object^>([this, wait](Object^, Object^) {
        wait->Stop();
        Capture::Start(_micId,
            [this](double level) {
                Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, level]() {
                    double w = Root->ActualWidth;
                    Meter->Width = std::max(0.0, std::min(w, w * level));
                }));
            },
            [this](const std::vector<int16_t>& pcm, bool heard, const std::wstring& error) {
                std::vector<int16_t> copy = pcm;
                std::wstring err = error;
                Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy, heard, err]() {
                    RecordSaved(copy, heard, err);
                }));
            });
    });
    wait->Start();
}

void MainPage::SaveTake(const std::wstring& file, const std::wstring& say, const std::vector<int16_t>& pcm) {
    std::string wav = assist::WavBytes(pcm, 16000);
    std::wstring ms = std::to_wstring(pcm.size() / 16);
    std::wstring line = _recSession + L"\t" + file + L"\t" + say + L"\t" + ms + L"\t" + Capture::LastFormat() + L"\t" +
                        (_micId.empty() ? std::wstring(L"predefinito") : _micId);
    ++_recSaved;
    Logger::Write(L"registra: " + file + L" \"" + say + L"\" " + ms + L" ms");
    auto bytes = ref new Array<unsigned char>(static_cast<unsigned int>(wav.size()));
    memcpy(bytes->Data, wav.data(), wav.size());
    String^ b64 = Windows::Security::Cryptography::CryptographicBuffer::EncodeToBase64String(
        Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(bytes));
    Logger::PostNow(L"voce", ref new String((line + L"\t" + std::wstring(b64->Data())).c_str()));
    String^ session = ref new String(_recSession.c_str());
    String^ fileName = ref new String(file.c_str());
    String^ tsv = ref new String((line + L"\r\n").c_str());
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"voce", CreationCollisionOption::OpenIfExists))
        .then([session](StorageFolder^ voce) { return voce->CreateFolderAsync(session, CreationCollisionOption::OpenIfExists); })
        .then([fileName, bytes, tsv](StorageFolder^ dir) {
            return create_task(dir->CreateFileAsync(fileName, CreationCollisionOption::ReplaceExisting))
                .then([bytes](StorageFile^ f) { return FileIO::WriteBytesAsync(f, bytes); })
                .then([dir]() { return dir->CreateFileAsync(L"elenco.tsv", CreationCollisionOption::OpenIfExists); })
                .then([tsv](StorageFile^ f) { return FileIO::AppendTextAsync(f, tsv); });
        })
        .then([](task<void> t) {
            try { t.get(); } catch (Exception^ ex) { Logger::Write(L"registra: salvataggio " + std::wstring(ex->Message->Data())); }
        });
}

// "registra lungo [nome]": the microphone stays open until the arrow (videos played from the PC speakers, for
// the Lumia microphone's own data); every 30 s the piece is saved and sent like a prompt's take ("lungo").
// "registra testo [nome]": the reading texts (tools/wsr/testo.py), one line per take; the session name starts
// with "testo".
void MainPage::StartTextRecording(const std::wstring& typed, const std::wstring& kind) {
    _recKind = kind;  // "testo" (Assets\\testo.txt) or "comandi" (Assets\\comandi.txt, grammar sentences)
    _busy = true;
    // "registra testo speaker 42": start from line 42; the name is kept as in the session folder (letters, digits)
    std::wstring speaker, number;
    size_t sp = typed.find_last_of(L' ');
    std::wstring last = sp == std::wstring::npos ? typed : typed.substr(sp + 1);
    bool digits = !last.empty() && last.find_first_not_of(L"0123456789") == std::wstring::npos;
    std::wstring name = digits ? (sp == std::wstring::npos ? std::wstring() : typed.substr(0, sp)) : typed;
    for (wchar_t c : name) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) speaker += c;
    }
    int from = digits ? _wtoi(last.c_str()) : 0;
    // "prova": a fresh batch of grammar sentences made on the PC (tools/wsr/prova.py nuovo -> share/prova.txt),
    // fetched from the inbox server; the others are files in Assets
    task<String^> source = kind == L"prova"
        ? create_task((ref new Windows::Web::Http::HttpClient())->GetStringAsync(ref new Uri(ref new String(
              (L"http://192.168.1.148:8765/prova.txt?t=" + std::to_wstring(GetTickCount64())).c_str()))))
        : create_task(Windows::ApplicationModel::Package::Current->InstalledLocation->GetFileAsync(
                          ref new String((L"Assets\\" + kind + L".txt").c_str())))
              .then([](StorageFile^ f) { return FileIO::ReadTextAsync(f); });
    source.then([this, speaker, from, kind](task<String^> t) {
            std::wstring text;
            try { text = t.get()->Data(); } catch (Exception^) {}
            _recList.clear();
            size_t a = 0;
            while (a < text.size()) {
                size_t b = text.find(L'\n', a);
                if (b == std::wstring::npos) b = text.size();
                std::wstring line = text.substr(a, b - a);
                size_t tab = line.find(L'\t');
                if (tab != std::wstring::npos) {
                    std::wstring say = line.substr(tab + 1);
                    if (!say.empty() && say.back() == L'\r') say.pop_back();
                    _recList.push_back(std::make_pair(say, say));
                }
                a = b + 1;
            }
            if (_recList.empty()) {
                _busy = false;
                SetState(kind + L".txt mancante");
                return;
            }
            _recText = true;
            StartRecording(_recKind + speaker);
            // resume where this speaker stopped last time (the takes keep their line numbers)
            auto values = ApplicationData::Current->LocalSettings->Values;
            String^ key = ref new String((_recKind + L".dopo." + speaker).c_str());
            if (from > 0) {
                if (static_cast<size_t>(from) <= _recList.size()) _recIndex = static_cast<size_t>(from - 1);
            } else if (kind != L"prova" && values->HasKey(key)) {  // a prova batch always starts from line 1
                size_t next = static_cast<size_t>(safe_cast<int>(values->Lookup(key)));
                if (next < _recList.size()) _recIndex = next;
            }
        }, task_continuation_context::use_current());
}

void MainPage::StartLongRecording(const std::wstring& speaker) {
    StartRecording(L"lungo" + speaker);  // session name and state; RecordNext is not used
}

void MainPage::RecordSaved(const std::vector<int16_t>& pcm, bool heard, const std::wstring& error) {
    _listening = false;
    Meter->Width = 0;
    Sounds::Play(Sounds::DoneListening);
    if (_recStop) {
        StopRecordingMode(L"fermato");
        return;
    }
    if (!error.empty()) {
        StopRecordingMode(L"microfono: " + error);
        return;
    }
    size_t index = _recIndex;
    if (!heard && _recRetries < 2) {  // nothing heard: the same prompt again
        ++_recRetries;
        Logger::Write(L"registra: " + std::to_wstring(index + 1) + L" non sentita, ripeto");
        RecordNext();
        return;
    }
    _recRetries = 0;
    ++_recIndex;
    if (_recText) {  // progress for "registra testo": the next run starts here
        std::wstring who = _recSession.substr(_recSession.find(L"-" + _recKind) + 1 + _recKind.size());
        ApplicationData::Current->LocalSettings->Values->Insert(ref new String((_recKind + L".dopo." + who).c_str()),
                                                               static_cast<int>(_recIndex));
    }
    if (heard) {
        wchar_t name[16];
        swprintf_s(name, L"%03u.wav", static_cast<unsigned>(index + 1));
        SaveTake(name, _recList[index].second, pcm);
    } else {
        Logger::Write(L"registra: " + std::to_wstring(index + 1) + L" saltata (non sentita)");
    }
    // a short pause before the next prompt
    auto pause = ref new DispatcherTimer();
    TimeSpan t;
    t.Duration = 6000000LL;
    pause->Interval = t;
    pause->Tick += ref new EventHandler<Object^>([this, pause](Object^, Object^) {
        pause->Stop();
        RecordNext();
    });
    pause->Start();
}
