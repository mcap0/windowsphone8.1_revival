#include "pch.h"
#include "VoskEngine.h"
#include "Logger.h"
#include "Capture.h"

#include <deque>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Core;

namespace {

const wchar_t* const ModelName = L"vosk-model-small-it-0.22";
const wchar_t* const ServerBase = L"http://192.168.1.148:8765/vosk/";

typedef void* (*ModelNewFn)(const char*);
typedef void* (*RecNewFn)(void*, float);
typedef int (*AcceptFn)(void*, const short*, int);
typedef const char* (*ResultFn)(void*);
typedef void (*FreeFn)(void*);
typedef void (*DelaysFn)(void*, float, float, float);
typedef void (*LogLevelFn)(int);
typedef void (*FatalFn)(void (*)(const char*));
typedef void (*LogFn)(void (*)(const char*));

ModelNewFn p_model_new;
RecNewFn p_rec_new;
AcceptFn p_accept;
ResultFn p_partial, p_final, p_result;
DelaysFn p_delays;
FreeFn p_rec_free;
LogLevelFn p_log_level;
FatalFn p_fatal;

void* g_model = nullptr;
volatile bool g_ready = false;
bool g_preparing = false;
std::wstring g_state = L"non preparato";

// one utterance at a time
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
std::deque<std::vector<int16_t>> g_queue;
bool g_ended = false;
bool g_running = false;
VoskEngine::TextFn g_fn;
CoreDispatcher^ g_dispatcher;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// the value of "key" in Vosk's small JSON ({"partial" : "..."} / {"text" : "..."})
std::wstring JsonText(const char* json, const char* key) {
    std::string j = json ? json : "";
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return std::wstring();
    p = j.find('"', j.find(':', p + k.size()) + 1);
    if (p == std::string::npos) return std::wstring();
    size_t e = j.find('"', p + 1);
    return Wide(j.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1));
}

// Kaldi's log lines while the model loads, with the ms since the load started (which phase is slow, PLAN.md §59.5)
ULONGLONG g_loadStart;
void KaldiLog(const char* what) {
    Logger::WriteUtf8(("vosk +" + std::to_string(GetTickCount64() - g_loadStart) + " ms: " + (what ? what : "")).c_str());
}

void Fatal(const char* what) {
    Logger::WriteUtf8((std::string("vosk: ERRORE FATALE ") + (what ? what : "?")).c_str());
    Logger::FlushNow();
}

// the preparation ended (model loaded or not): a worker started before the model was ready stops waiting
void PrepareDone() {
    AcquireSRWLockExclusive(&g_lock);
    g_preparing = false;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
}

void Post(const std::wstring& text, bool final) {
    VoskEngine::TextFn fn = g_fn;
    if (!fn || g_dispatcher == nullptr) return;
    g_dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([fn, text, final]() { fn(text, final); }));
}

void Worker() {
    // listening may start while the model is still loading: the audio queues up and is decoded once it is ready
    ULONGLONG w0 = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    while (!g_ready && g_preparing) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
    size_t queued = g_queue.size();
    ReleaseSRWLockExclusive(&g_lock);
    if (GetTickCount64() - w0 > 50)
        Logger::Write(L"vosk: ascolto partito prima del modello, atteso " + std::to_wstring(GetTickCount64() - w0) +
                      L" ms (" + std::to_wstring(queued) + L" pezzi di audio in coda)");
    if (!g_ready) {
        Logger::Write(L"vosk: modello non pronto, niente testo");
        Capture::StopNow();  // the capture waits for Vosk's endpoint, which will not come
        AcquireSRWLockExclusive(&g_lock);
        g_running = false;
        g_queue.clear();
        ReleaseSRWLockExclusive(&g_lock);
        Post(std::wstring(), true);
        return;
    }
    void* rec = p_rec_new(g_model, 16000.0f);
    // the end of the utterance is Vosk's endpoint (silence phones after the words), not the energy detector: on the
    // Lumia's microphone quiet syllables (150-500 rms) and the room (up to ~270) overlap, and that detector cut
    // sentences mid-word (PLAN.md §59.6). 8 s without speech, then 1.0 s of decoded silence after a word that can
    // end a sentence (+0.5 / +1.0 s for weaker endings), 25 s at most. On the PC's 100 commands it never ended one
    // early.
    p_delays(rec, 8.0f, 1.0f, 25.0f);
    std::wstring endpointText;
    bool endpoint = false;
    std::wstring last;
    ULONGLONG busy = 0;
    size_t samples = 0;
    // everything queued goes to Vosk in one go, and the partial text is asked at most every 200 ms of audio: the
    // capture delivers ~11 ms pieces, and a partial result per piece cost ~20% of the decoding (and the backlog of
    // audio heard while the model was loading never caught up)
    size_t sincePartial = 0;
    for (;;) {
        std::vector<int16_t> piece;
        bool ended, more;
        AcquireSRWLockExclusive(&g_lock);
        while (g_queue.empty() && !g_ended) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
        ended = g_ended && g_queue.empty();
        while (!g_queue.empty() && piece.size() < 3200) {
            piece.insert(piece.end(), g_queue.front().begin(), g_queue.front().end());
            g_queue.pop_front();
        }
        more = !g_queue.empty();
        ReleaseSRWLockExclusive(&g_lock);
        if (ended) break;
        ULONGLONG t0 = GetTickCount64();
        bool atEndpoint = p_accept(rec, piece.data(), (int)piece.size()) == 1;
        samples += piece.size();
        if (atEndpoint) {
            endpointText = JsonText(p_result(rec), "text");
            endpoint = true;
            busy += GetTickCount64() - t0;
            Logger::Write(L"vosk: fine frase (endpoint) a " + std::to_wstring(samples / 16) + L" ms di audio");
            AcquireSRWLockExclusive(&g_lock);
            g_ended = true;  // the audio still coming until the capture stops is not part of this utterance
            g_queue.clear();
            ReleaseSRWLockExclusive(&g_lock);
            Capture::StopNow();
            break;
        }
        sincePartial += piece.size();
        std::wstring part;
        bool asked = sincePartial >= 3200 || (!more && sincePartial >= 1600);
        if (asked) {
            part = JsonText(p_partial(rec), "partial");
            sincePartial = 0;
        }
        busy += GetTickCount64() - t0;
        if (asked && part != last) {
            last = part;
            Post(part, false);
        }
    }
    ULONGLONG t0 = GetTickCount64();
    std::wstring text = endpoint ? endpointText : JsonText(p_final(rec), "text");
    busy += GetTickCount64() - t0;
    p_rec_free(rec);
    wchar_t info[128];
    swprintf_s(info, L"vosk: %u ms di audio, %u ms di calcolo (%.2fx)", (unsigned)(samples / 16), (unsigned)busy,
               samples ? busy / (samples / 16.0) : 0.0);
    Logger::Write(std::wstring(info) + L": \"" + text + L"\"");
    AcquireSRWLockExclusive(&g_lock);
    g_running = false;
    ReleaseSRWLockExclusive(&g_lock);
    Post(text, true);
}

task<void> Download(StorageFolder^ dir, const std::wstring& rel, unsigned long long size) {
    // folders of rel ("graph/HCLr.fst")
    std::wstring folder, name = rel;
    size_t slash = rel.find_last_of(L'/');
    if (slash != std::wstring::npos) { folder = rel.substr(0, slash); name = rel.substr(slash + 1); }
    for (auto& c : folder) if (c == L'/') c = L'\\';
    task<StorageFolder^> where = folder.empty() ? task_from_result(dir)
        : create_task(dir->CreateFolderAsync(ref new String(folder.c_str()), CreationCollisionOption::OpenIfExists));
    String^ uri = ref new String((std::wstring(ServerBase) + ModelName + L"/" + rel).c_str());
    String^ fname = ref new String(name.c_str());
    return where.then([uri, fname, size](StorageFolder^ f) {
        return create_task(f->CreateFileAsync(fname, CreationCollisionOption::ReplaceExisting))
            .then([uri](StorageFile^ file) {
                auto client = ref new Windows::Web::Http::HttpClient();
                return create_task(client->GetInputStreamAsync(ref new Windows::Foundation::Uri(uri)))
                    .then([file](IInputStream^ in) {
                        return create_task(file->OpenAsync(FileAccessMode::ReadWrite)).then([in](IRandomAccessStream^ out) {
                            return create_task(RandomAccessStream::CopyAndCloseAsync(in, out->GetOutputStreamAt(0)));
                        });
                    });
            })
            .then([](unsigned long long) {});
    });
}

}  // namespace

bool VoskEngine::Ready() { return g_ready; }
bool VoskEngine::Coming() {
    AcquireSRWLockShared(&g_lock);
    bool c = g_ready || g_preparing;
    ReleaseSRWLockShared(&g_lock);
    return c;
}
std::wstring VoskEngine::State() { return g_state; }

void VoskEngine::Prepare() {
    if (g_preparing || g_ready) return;
    // a load that never finished (the app died in it): do not try again until a new version
    auto values = ApplicationData::Current->LocalSettings->Values;
    auto v = Windows::ApplicationModel::Package::Current->Id->Version;
    String^ key = ref new String((L"vosk.caricamento." + std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." +
                                  std::to_wstring(v.Build)).c_str());
    if (values->HasKey(key)) {
        Logger::Write(L"vosk: il caricamento precedente non era finito (app chiusa): Vosk spento in questa versione");
        g_state = L"spento dopo un errore";
        return;
    }
    g_preparing = true;
    g_dispatcher = Windows::UI::Core::CoreWindow::GetForCurrentThread()->Dispatcher;
    ULONGLONG t0 = GetTickCount64();
    StorageFolder^ local = ApplicationData::Current->LocalFolder;
    g_state = L"controllo il modello";
    create_task(local->CreateFolderAsync(ref new String(ModelName), CreationCollisionOption::OpenIfExists))
        .then([t0](StorageFolder^ dir) {
            // the list of files with sizes; a file whose size differs is fetched again
            auto client = ref new Windows::Web::Http::HttpClient();
            String^ uri = ref new String((std::wstring(ServerBase) + ModelName + L"/files.txt").c_str());
            return create_task(client->GetStringAsync(ref new Windows::Foundation::Uri(uri)))
                .then([dir](task<String^> t) {
                    std::wstring list;
                    try { list = t.get()->Data(); } catch (Exception^) {}
                    std::vector<std::pair<std::wstring, unsigned long long>> files;
                    size_t a = 0;
                    while (a < list.size()) {
                        size_t b = list.find(L'\n', a);
                        if (b == std::wstring::npos) b = list.size();
                        std::wstring line = list.substr(a, b - a);
                        size_t sp = line.find_last_of(L' ');
                        if (sp != std::wstring::npos) files.push_back(std::make_pair(line.substr(0, sp), _wtoi64(line.c_str() + sp + 1)));
                        a = b + 1;
                    }
                    // check sizes on disk (Win32: the LocalFolder is ours)
                    std::wstring base = std::wstring(dir->Path->Data()) + L"\\";
                    std::vector<std::pair<std::wstring, unsigned long long>> missing;
                    for (auto& f : files) {
                        std::wstring p = base + f.first;
                        for (auto& c : p) if (c == L'/') c = L'\\';
                        WIN32_FILE_ATTRIBUTE_DATA d;
                        // a file that does not exist is missing even when it should be empty (online_cmvn.conf is 0 bytes)
                        bool exists = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) != 0;
                        unsigned long long have = exists ? ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow : 0;
                        if (!exists || have != f.second) missing.push_back(f);
                    }
                    if (files.empty()) Logger::Write(L"vosk: elenco del modello non raggiungibile (PC acceso?)");
                    Logger::Write(L"vosk: " + std::to_wstring(missing.size()) + L" file del modello da scaricare");
                    task<void> chain = task_from_result();
                    for (auto& f : missing) {
                        auto one = f;
                        chain = chain.then([dir, one]() {
                            g_state = L"scarico " + one.first;
                            Logger::Write(L"vosk: scarico " + one.first + L" (" + std::to_wstring(one.second / 1024) + L" KB)");
                            return Download(dir, one.first, one.second);
                        });
                    }
                    return chain.then([dir, files]() { return std::make_pair(dir, !files.empty()); });
                });
        })
        .then([t0, key](task<std::pair<StorageFolder^, bool>> t) {
            StorageFolder^ dir;
            try { dir = t.get().first; } catch (Exception^ ex) {
                Logger::Write(L"vosk: download fallito: " + std::wstring(ex->Message->Data()));
                g_state = L"download fallito";
                PrepareDone();
                return;
            }
            std::string path = Utf8(dir->Path->Data());
            Windows::System::Threading::ThreadPool::RunAsync(ref new Windows::System::Threading::WorkItemHandler(
                [path, t0, key](Windows::Foundation::IAsyncAction^) {
                    g_state = L"carico il modello";
                    HMODULE dll = LoadPackagedLibrary(L"vosk.dll", 0);
                    if (!dll) {
                        Logger::Write(L"vosk: LoadPackagedLibrary fallito " + std::to_wstring(GetLastError()));
                        g_state = L"vosk.dll non caricata";
                        PrepareDone();
                        return;
                    }
                    p_model_new = (ModelNewFn)GetProcAddress(dll, "vosk_model_new");
                    p_rec_new = (RecNewFn)GetProcAddress(dll, "vosk_recognizer_new");
                    p_accept = (AcceptFn)GetProcAddress(dll, "vosk_recognizer_accept_waveform_s");
                    p_partial = (ResultFn)GetProcAddress(dll, "vosk_recognizer_partial_result");
                    p_final = (ResultFn)GetProcAddress(dll, "vosk_recognizer_final_result");
                    p_result = (ResultFn)GetProcAddress(dll, "vosk_recognizer_result");
                    p_delays = (DelaysFn)GetProcAddress(dll, "vosk_recognizer_set_endpointer_delays");
                    p_rec_free = (FreeFn)GetProcAddress(dll, "vosk_recognizer_free");
                    p_log_level = (LogLevelFn)GetProcAddress(dll, "vosk_set_log_level");
                    p_fatal = (FatalFn)GetProcAddress(dll, "vosk_wp81_set_fatal_handler");
                    if (!p_model_new || !p_rec_new || !p_accept || !p_partial || !p_final || !p_result || !p_delays ||
                        !p_rec_free) {
                        Logger::Write(L"vosk: funzioni mancanti nella DLL");
                        g_state = L"vosk.dll incompleta";
                        PrepareDone();
                        return;
                    }
                    if (p_fatal) p_fatal(&Fatal);
                    if (p_log_level) p_log_level(0);
                    ULONGLONG t1 = GetTickCount64();
                    g_loadStart = t1;
                    LogFn setLog = (LogFn)GetProcAddress(dll, "vosk_wp81_set_log_handler");
                    if (setLog) setLog(&KaldiLog);
                    ApplicationData::Current->LocalSettings->Values->Insert(key, 1);
                    g_model = p_model_new(path.c_str());
                    ApplicationData::Current->LocalSettings->Values->Remove(key);
                    if (setLog) setLog(nullptr);
                    Logger::Write(L"vosk: modello " + std::wstring(g_model ? L"caricato" : L"NON caricato") + L" in " +
                                  std::to_wstring(GetTickCount64() - t1) + L" ms (preparazione totale " +
                                  std::to_wstring(GetTickCount64() - t0) + L" ms)");
                    g_ready = g_model != nullptr;
                    g_state = g_ready ? L"pronto" : L"modello non caricato";
                    PrepareDone();
                }));
        });
}

void VoskEngine::Begin(TextFn fn) {
    if (!Coming()) return;
    AcquireSRWLockExclusive(&g_lock);
    bool busy = g_running;
    if (!busy) {
        g_queue.clear();
        g_ended = false;
        g_running = true;
        g_fn = fn;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (busy) return;
    Windows::System::Threading::ThreadPool::RunAsync(ref new Windows::System::Threading::WorkItemHandler(
        [](Windows::Foundation::IAsyncAction^) { Worker(); }),
        Windows::System::Threading::WorkItemPriority::High, Windows::System::Threading::WorkItemOptions::TimeSliced);
}

void VoskEngine::Feed(const std::vector<int16_t>& pcm) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_running && !g_ended) g_queue.push_back(pcm);
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_cv);
}

void VoskEngine::End() {
    AcquireSRWLockExclusive(&g_lock);
    g_ended = true;
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_cv);
}
