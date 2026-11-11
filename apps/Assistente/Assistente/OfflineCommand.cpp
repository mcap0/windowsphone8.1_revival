#include "pch.h"
#include "OfflineCommand.h"
#include "Logger.h"

#include <memory>
#include <set>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media::SpeechRecognition;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Xaml;

namespace {

assist::Wsp g_net;
SpeechRecognizer^ g_recognizer;
SpeechRecognizer^ g_next;  // rebuilt with fresh names while the old one was listening: swapped at the next Start
bool g_preparing = false;
IAsyncOperation<SpeechRecognitionResult^>^ g_op;
std::wstring g_heard;  // "" while listening or when not sure
bool g_done = true;
ULONGLONG g_started = 0;
std::wstring g_template;  // Assets/comandi.grxml
std::wstring g_status = L"riconoscitore non ancora preparato";

const wchar_t* const ContactsCache = L"contatti.txt";
const wchar_t* const MusicCache = L"brani.txt";
const size_t MaxNames = 3000;

std::wstring Ms(ULONGLONG since) { return std::to_wstring(GetTickCount64() - since) + L" ms"; }

std::wstring Lower(const std::wstring& s) {
    std::wstring r(s);
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return r;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L"&amp;";
        else if (c == L'<') r += L"&lt;";
        else if (c == L'>') r += L"&gt;";
        else if (c == L'"') r += L"&quot;";
        else if (c >= 32) r += c;
    }
    return r;
}

std::set<std::wstring> Lines(const std::wstring& text) {
    std::set<std::wstring> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = Trim(text.substr(start, nl - start));
        if (!line.empty()) out.insert(line);
        start = nl + 1;
    }
    return out;
}

std::wstring Joined(const std::set<std::wstring>& names) {
    std::wstring t;
    for (const auto& n : names) t += n + L"\n";
    return t;
}

std::wstring Rule(const wchar_t* id, const std::set<std::wstring>& names) {
    std::wstring r = L"<rule id=\"" + std::wstring(id) + L"\">";
    if (names.empty()) return r + L"<ruleref special=\"VOID\"/></rule>\n";
    r += L"<one-of>";
    size_t n = 0;
    for (const auto& name : names) {
        if (++n > MaxNames) break;
        r += L"<item>" + XmlEscape(name) + L"</item>";
    }
    return r + L"</one-of></rule>\n";
}

task<String^> ReadLocal(const wchar_t* name) {
    // no TryGetItemAsync on the phone: a missing file is the exception, caught below
    return create_task(ApplicationData::Current->LocalFolder->GetFileAsync(ref new String(name)))
        .then([](StorageFile^ file) { return create_task(FileIO::ReadTextAsync(file)); })
        .then([](task<String^> t) {
            try { return t.get(); } catch (Exception^) { return ref new String(); }
        });
}

task<void> WriteLocal(const wchar_t* name, const std::wstring& text) {
    return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(ref new String(name),
                                                                               CreationCollisionOption::ReplaceExisting))
        .then([text](StorageFile^ file) { return create_task(FileIO::WriteTextAsync(file, ref new String(text.c_str()))); });
}

// the grammar with these names, compiled into a recognizer (nullptr if it does not compile)
task<SpeechRecognizer^> Build(std::shared_ptr<std::set<std::wstring>> contacts,
                              std::shared_ptr<std::set<std::wstring>> music, ULONGLONG t0) {
    std::wstring g = g_template;
    size_t at = g.find(L"<!--DINAMICO-->");
    if (at == std::wstring::npos) return task_from_result<SpeechRecognizer^>(nullptr);
    g.replace(at, 15, Rule(L"contatto", *contacts) + Rule(L"brano", *music));
    return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(L"comandi.grxml",
                                                                               CreationCollisionOption::ReplaceExisting))
        .then([g](StorageFile^ file) {
            return create_task(FileIO::WriteTextAsync(file, ref new String(g.c_str()))).then([file]() { return file; });
        })
        .then([contacts, music, t0](StorageFile^ file) {
            auto r = ref new SpeechRecognizer(ref new Windows::Globalization::Language(L"it-IT"));
            r->Constraints->Append(ref new SpeechRecognitionGrammarFileConstraint(file, L"comandi"));
            TimeSpan t;
            t.Duration = 80000000LL;  // 8 s of silence at the start: our capture decides when to stop anyway
            r->Timeouts->InitialSilenceTimeout = t;
            return create_task(r->CompileConstraintsAsync()).then([r, contacts, music, t0](SpeechRecognitionCompilationResult^ res) -> SpeechRecognizer^ {
                bool ok = res->Status == SpeechRecognitionResultStatus::Success;
                std::wstring line = L"grammatica con " + std::to_wstring(contacts->size()) + L" contatti e " +
                                    std::to_wstring(music->size()) + L" brani " + (ok ? L"compilata" : L"NON compilata, stato " +
                                    std::to_wstring(static_cast<int>(res->Status))) + L", " + Ms(t0);
                Logger::Write(L"offline: " + line);
                g_status = line;
                if (!ok) return nullptr;
                return r;
            });
        });
}

// contacts (display and first names) and the music library's artists and albums, read fresh
task<void> Refresh(std::shared_ptr<std::set<std::wstring>> oldContacts, std::shared_ptr<std::set<std::wstring>> oldMusic) {
    using namespace Windows::ApplicationModel::Contacts;
    auto contacts = std::make_shared<std::set<std::wstring>>();
    auto music = std::make_shared<std::set<std::wstring>>();
    ULONGLONG t0 = GetTickCount64();
    return create_task(ContactManager::RequestStoreAsync())
        .then([](ContactStore^ store) { return create_task(store->FindContactsAsync()); })
        .then([contacts, t0](task<IVectorView<Contact^>^> t) {
            try {
                auto list = t.get();
                for (unsigned i = 0; i < list->Size; ++i) {
                    Contact^ c = list->GetAt(i);
                    if (c->Phones->Size == 0) continue;
                    contacts->insert(Lower(Trim(c->DisplayName->Data())));
                    // "chiama Francesca": Actions::Call then lists every Francesca
                    if (c->FirstName != nullptr && c->FirstName->Length() > 0) contacts->insert(Lower(Trim(c->FirstName->Data())));
                }
                contacts->erase(std::wstring());
                Logger::Write(L"offline: " + std::to_wstring(contacts->size()) + L" nomi dai contatti, " + Ms(t0));
            } catch (Exception^ ex) {
                Logger::Write(L"offline: contatti " + std::wstring(ex->Message->Data()));
            }
            using namespace Windows::Storage::Search;
            return create_task(KnownFolders::MusicLibrary->CreateFolderQuery(CommonFolderQuery::GroupByArtist)->GetFoldersAsync());
        })
        .then([music](task<IVectorView<StorageFolder^>^> t) {
            try {
                for (StorageFolder^ f : t.get()) music->insert(Trim(f->DisplayName->Data()));
            } catch (Exception^ ex) {
                Logger::Write(L"offline: artisti " + std::wstring(ex->Message->Data()));
            }
            using namespace Windows::Storage::Search;
            return create_task(KnownFolders::MusicLibrary->CreateFolderQuery(CommonFolderQuery::GroupByAlbum)->GetFoldersAsync());
        })
        .then([contacts, music, oldContacts, oldMusic, t0](task<IVectorView<StorageFolder^>^> t) -> task<void> {
            try {
                for (StorageFolder^ f : t.get()) music->insert(Trim(f->DisplayName->Data()));
            } catch (Exception^ ex) {
                Logger::Write(L"offline: album " + std::wstring(ex->Message->Data()));
            }
            music->erase(std::wstring());
            Logger::Write(L"offline: " + std::to_wstring(music->size()) + L" artisti e album, " + Ms(t0));
            if (*contacts == *oldContacts && *music == *oldMusic) return task_from_result();
            return WriteLocal(ContactsCache, Joined(*contacts))
                .then([music]() { return WriteLocal(MusicCache, Joined(*music)); })
                .then([contacts, music]() { return Build(contacts, music, GetTickCount64()); })
                .then([](SpeechRecognizer^ r) {
                    if (r == nullptr) return;
                    if (g_done) g_recognizer = r;
                    else g_next = r;
                });
        });
}

// diagnostics of 2026-11-03 (RecognizeAsync fails with 0x800455A0 even alone): what the system offers
void Diagnose() {
    try {
        std::wstring langs;
        for (auto l : SpeechRecognizer::SupportedGrammarLanguages) langs += std::wstring(l->LanguageTag->Data()) + L" ";
        std::wstring topics;
        for (auto l : SpeechRecognizer::SupportedTopicLanguages) topics += std::wstring(l->LanguageTag->Data()) + L" ";
        Logger::Write(L"diagnosi: lingua di sistema " + std::wstring(SpeechRecognizer::SystemSpeechLanguage->LanguageTag->Data()) +
                      L", grammatiche: " + langs + L", dettatura: " + topics);
    } catch (Exception^ ex) {
        Logger::Write(L"diagnosi lingue: " + std::wstring(ex->Message->Data()));
    }
    const wchar_t* dirs[] = {L"C:\\Data\\SharedData\\Speech\\it-it", L"C:\\Data\\SharedData\\Speech\\en-us"};
    for (const wchar_t* dir : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((std::wstring(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) {
            Logger::Write(L"diagnosi: " + std::wstring(dir) + L" non leggibile, errore " + std::to_wstring(GetLastError()));
            continue;
        }
        std::wstring list;
        int n = 0;
        do {
            if (fd.cFileName[0] == L'.') continue;
            ++n;
            list += std::wstring(fd.cFileName) + L"=" + std::to_wstring((static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow) + L" ";
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        Logger::Write(L"diagnosi: " + std::wstring(dir) + L" " + std::to_wstring(n) + L" file: " + list);
    }
}

// the factory English recognizer with a tiny list: does speech recognition work at all on this phone?
void TryEnglish() {
    try {
        auto r = ref new SpeechRecognizer(ref new Windows::Globalization::Language(L"en-US"));
        auto words = ref new Platform::Collections::Vector<String^>();
        words->Append(L"hello");
        words->Append(L"call mom");
        r->Constraints->Append(ref new SpeechRecognitionListConstraint(words, L"prova"));
        ULONGLONG t0 = GetTickCount64();
        create_task(r->CompileConstraintsAsync())
            .then([r, t0](SpeechRecognitionCompilationResult^ c) {
                Logger::Write(L"diagnosi inglese: compilata, stato " + std::to_wstring(static_cast<int>(c->Status)) + L", " + Ms(t0));
                return create_task(r->RecognizeAsync());
            })
            .then([r, t0](task<SpeechRecognitionResult^> t) {
                try {
                    auto res = t.get();
                    Logger::Write(L"diagnosi inglese: sentito \"" + std::wstring(res->Text->Data()) + L"\" stato " +
                                  std::to_wstring(static_cast<int>(res->Status)) + L", " + Ms(t0));
                } catch (Exception^ ex) {
                    Logger::Write(L"diagnosi inglese: " + std::wstring(ex->Message->Data()) + L" " +
                                  std::to_wstring(static_cast<unsigned>(ex->HResult)) + L", " + Ms(t0));
                }
            });
    } catch (Exception^ ex) {
        Logger::Write(L"diagnosi inglese: non parte, " + std::wstring(ex->Message->Data()) + L" " +
                      std::to_wstring(static_cast<unsigned>(ex->HResult)));
    }
}

}  // namespace

const assist::Wsp& OfflineCommand::Net() { return g_net; }

std::wstring OfflineCommand::Status() { return g_status; }

void OfflineCommand::Prepare() {
    if (g_recognizer != nullptr || g_preparing) return;
    g_preparing = true;
    ULONGLONG t0 = GetTickCount64();
    auto contacts = std::make_shared<std::set<std::wstring>>();
    auto music = std::make_shared<std::set<std::wstring>>();
    auto installed = Windows::ApplicationModel::Package::Current->InstalledLocation;
    Logger::Write(L"offline: preparo");
    Diagnose();
    create_task(installed->GetFileAsync(L"Assets\\intent.wsp"))
        .then([](StorageFile^ f) { return create_task(FileIO::ReadBufferAsync(f)); })
        .then([t0](IBuffer^ buffer) {
            auto reader = DataReader::FromBuffer(buffer);
            std::string bytes(buffer->Length, '\0');
            if (buffer->Length) reader->ReadBytes(ArrayReference<unsigned char>(reinterpret_cast<unsigned char*>(&bytes[0]), buffer->Length));
            std::string error;
            bool ok = g_net.Load(bytes, error);
            Logger::Write(L"wsp: rete " + std::wstring(ok ? L"caricata" : L"NON caricata: ") +
                          std::wstring(error.begin(), error.end()) + L" (" + std::to_wstring(bytes.size()) + L" byte, " + Ms(t0) + L")");
        })
        .then([installed]() { return create_task(installed->GetFileAsync(L"Assets\\comandi.grxml")); })
        .then([](StorageFile^ f) { return create_task(FileIO::ReadTextAsync(f)); })
        .then([](String^ text) {
            g_template = text->Data();
            return ReadLocal(ContactsCache);
        })
        .then([contacts](String^ text) {
            *contacts = Lines(text->Data());
            return ReadLocal(MusicCache);
        })
        .then([contacts, music, t0](String^ text) {
            *music = Lines(text->Data());
            Logger::Write(L"offline: cache " + std::to_wstring(contacts->size()) + L" contatti, " +
                          std::to_wstring(music->size()) + L" brani, " + Ms(t0));
            return Build(contacts, music, t0);
        })
        .then([contacts, music, t0](SpeechRecognizer^ r) -> task<SpeechRecognizer^> {
            if (r != nullptr || (contacts->empty() && music->empty())) return task_from_result(r);
            // a name the recognizer does not take: without the names, the commands still work
            Logger::Write(L"offline: riprovo senza i nomi");
            return Build(std::make_shared<std::set<std::wstring>>(), std::make_shared<std::set<std::wstring>>(), t0);
        })
        .then([contacts, music, t0](task<SpeechRecognizer^> t) {
            try {
                g_recognizer = t.get();
                if (g_recognizer != nullptr) Logger::Write(L"offline: pronto in " + Ms(t0));
            } catch (Exception^ ex) {
                g_status = L"preparazione: " + std::wstring(ex->Message->Data()) + L" " +
                           std::to_wstring(static_cast<unsigned>(ex->HResult));
                Logger::Write(L"offline: " + g_status);
            }
            g_preparing = false;
            return Refresh(contacts, music);
        })
        .then([](task<void> t) {
            try { t.get(); }
            catch (Exception^ ex) { Logger::Write(L"offline: aggiornamento nomi " + std::wstring(ex->Message->Data())); }
            g_preparing = false;
        });
}

void OfflineCommand::WhenReady(int maxMs, std::function<void()> then) {
    if (g_recognizer != nullptr || !g_preparing) {
        then();
        return;
    }
    ULONGLONG t0 = GetTickCount64();
    auto timer = ref new DispatcherTimer();
    TimeSpan step;
    step.Duration = 500000LL;  // 50 ms
    timer->Interval = step;
    timer->Tick += ref new EventHandler<Object^>([timer, then, t0, maxMs](Object^, Object^) {
        bool ready = g_recognizer != nullptr || !g_preparing;
        if (!ready && GetTickCount64() - t0 < static_cast<ULONGLONG>(maxMs)) return;
        timer->Stop();
        Logger::Write(L"offline: " + std::wstring(ready ? L"pronto dopo " : L"non pronto dopo ") + Ms(t0) + L" di attesa");
        then();
    });
    timer->Start();
}

void OfflineCommand::Start() {
    g_heard.clear();
    g_done = true;
    g_op = nullptr;
    if (g_next != nullptr) {
        g_recognizer = g_next;
        g_next = nullptr;
    }
    if (g_recognizer == nullptr) {
        g_status = L"riconoscitore non pronto (" + g_status + L")";
        Logger::Write(L"offline: " + g_status);
        return;
    }
    try {
        g_done = false;
        g_started = GetTickCount64();
        auto op = g_recognizer->RecognizeAsync();
        g_op = op;
        create_task(op).then([op](task<SpeechRecognitionResult^> t) {
            std::wstring heard;
            try {
                SpeechRecognitionResult^ r = t.get();
                std::wstring text = r->Text != nullptr ? r->Text->Data() : L"";
                static const wchar_t* const conf[] = {L"alta", L"media", L"bassa", L"rifiutato"};
                int ci = static_cast<int>(r->Confidence);
                g_status = L"sentito \"" + text + L"\", fiducia " + (ci >= 0 && ci < 4 ? conf[ci] : L"?") +
                           L", stato " + std::to_wstring(static_cast<int>(r->Status)) + L", " + Ms(g_started);
                Logger::Write(L"offline: " + g_status);
                // High = 0, Medium = 1: Low and Rejected are guesses the grammar forces on any sentence
                if (r->Status == SpeechRecognitionResultStatus::Success &&
                    (r->Confidence == SpeechRecognitionConfidence::High || r->Confidence == SpeechRecognitionConfidence::Medium))
                    heard = text;
            } catch (task_canceled&) {
            } catch (Exception^ ex) {
                g_status = L"riconoscitore: " + std::wstring(ex->Message->Data()) + L" " +
                           std::to_wstring(static_cast<unsigned>(ex->HResult));
                Logger::Write(L"offline: " + g_status);
            }
            if (g_op == op) {
                g_heard = heard;
                g_done = true;
            }
        });
    } catch (Exception^ ex) {
        g_done = true;
        g_status = L"il riconoscitore non parte: " + std::wstring(ex->Message->Data()) + L" " +
                   std::to_wstring(static_cast<unsigned>(ex->HResult));
        Logger::Write(L"offline: " + g_status);
    }
}

void OfflineCommand::ListenAlone(std::function<void(const std::wstring& text)> then) {
    if (g_next != nullptr) {
        g_recognizer = g_next;
        g_next = nullptr;
    }
    if (g_recognizer == nullptr) {
        g_status = L"riconoscitore non pronto (" + g_status + L")";
        Logger::Write(L"offline: " + g_status);
        then(std::wstring());
        return;
    }
    TimeSpan end;
    end.Duration = 12000000LL;  // 1.2 s of silence ends the sentence
    try { g_recognizer->Timeouts->EndSilenceTimeout = end; } catch (Exception^) {}
    g_done = false;
    g_started = GetTickCount64();
    Logger::Write(L"offline: ascolto solo col riconoscitore");
    try {
        auto op = g_recognizer->RecognizeAsync();
        g_op = op;
        create_task(op).then([op, then](task<SpeechRecognitionResult^> t) {
            std::wstring heard;
            try {
                SpeechRecognitionResult^ r = t.get();
                std::wstring text = r->Text != nullptr ? r->Text->Data() : L"";
                static const wchar_t* const conf[] = {L"alta", L"media", L"bassa", L"rifiutato"};
                int ci = static_cast<int>(r->Confidence);
                g_status = L"sentito \"" + text + L"\", fiducia " + (ci >= 0 && ci < 4 ? conf[ci] : L"?") +
                           L", stato " + std::to_wstring(static_cast<int>(r->Status)) + L", " + Ms(g_started);
                Logger::Write(L"offline: " + g_status);
                if (r->Status == SpeechRecognitionResultStatus::Success &&
                    (r->Confidence == SpeechRecognitionConfidence::High || r->Confidence == SpeechRecognitionConfidence::Medium))
                    heard = text;
            } catch (task_canceled&) {
                g_status = L"ascolto interrotto";
            } catch (Exception^ ex) {
                g_status = L"riconoscitore da solo: " + std::wstring(ex->Message->Data()) + L" " +
                           std::to_wstring(static_cast<unsigned>(ex->HResult)) + L", " + Ms(g_started);
                Logger::Write(L"offline: " + g_status);
                static bool tried = false;
                if (!tried) {
                    tried = true;
                    TryEnglish();
                }
            }
            g_done = true;
            if (g_op == op) g_op = nullptr;
            then(heard);
        });
    } catch (Exception^ ex) {
        g_done = true;
        g_status = L"il riconoscitore non parte: " + std::wstring(ex->Message->Data()) + L" " +
                   std::to_wstring(static_cast<unsigned>(ex->HResult));
        Logger::Write(L"offline: " + g_status);
        then(std::wstring());
    }
}

void OfflineCommand::CancelAlone() {
    if (g_op != nullptr) g_op->Cancel();
}

void OfflineCommand::Finish(std::function<void(const std::wstring& text)> then) {
    if (g_done) {
        then(g_heard);
        return;
    }
    // the recognizer stops on its own silence, a moment after our capture: up to 1.5 s
    auto timer = ref new DispatcherTimer();
    auto ticks = std::make_shared<int>(0);
    TimeSpan step;
    step.Duration = 1000000LL;
    timer->Interval = step;
    timer->Tick += ref new EventHandler<Object^>([timer, ticks, then](Object^, Object^) {
        if (!g_done && ++*ticks < 15) return;
        timer->Stop();
        if (!g_done) {
            g_status = L"il riconoscitore non ha risposto in tempo";
            Logger::Write(L"offline: " + g_status);
            if (g_op != nullptr) g_op->Cancel();
            g_op = nullptr;
            g_done = true;
            g_heard.clear();
        }
        then(g_heard);
    });
    timer->Start();
}
