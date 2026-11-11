// Internet: one tab drawn by our engine (webkit.dll, engine/) into a SwapChainPanel, an address box, touch on its own
// input thread, the compose bar above the keyboard, and the engine's events logged to share/inbox/internet.txt
// (tools/inbox on the PC). The engine is loaded at run time, so a missing DLL or import is reported instead of killing
// the app at start.
#include "pch.h"
#include <cmath>
#include "MainPage.xaml.h"
#include "Site.h"
#include "Vault.h"
#include <winsock2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace Browser;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace Windows::Storage;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Web::Http;

namespace { std::wstring PinTileId(const std::wstring& url); }

namespace
{
    // "Apri senza PIN": the vault is sealed with this fixed secret instead of a PIN, so it opens by itself; it stays
    // bound to the phone by DPAPI (a copy of the file elsewhere is useless), but anyone holding the phone gets in.
    const wchar_t* const noPinSecret = L"dumbphone-senza-pin";

    bool NoPin()
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        return values->HasKey(L"noPin") && safe_cast<bool>(values->Lookup(L"noPin"));
    }
}

namespace
{
    const wchar_t* const startPage = L"http://192.168.1.148:8765/browser/";
    const unsigned short serverPort = 8765;

    // Sites see a phone: Safari on an iPhone, the browser our engine is closest to, so pages send their mobile layout
    // (lighter: m.youtube.com instead of the desktop app). "Versione desktop" in the menu switches a site to Safari on
    // a Mac; WhatsApp Web and Telegram Web always get it, since they refuse phones and must see a real desktop browser.
    const wchar_t* const mobileUserAgent = L"Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1";
    const wchar_t* const desktopUserAgent = L"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
    const wchar_t* const alwaysDesktopSites[] = { L"web.whatsapp.com", L"web.telegram.org" };

    // "ua <name>" in the address box: the browser one site sees, for sites that refuse ours (Google's sign-in says
    // "this browser may not be secure"). Versions as of October 2026.
    struct Identity { const wchar_t* name; const wchar_t* agent; };
    const Identity identities[] = {
        { L"iphone", nullptr }, // the default (mobileUserAgent)
        { L"ipad", L"Mozilla/5.0 (iPad; CPU OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1" },
        { L"mac", L"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15" },
        { L"linux", L"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15" },
        { L"firefox", L"Mozilla/5.0 (Android 15; Mobile; rv:156.0) Gecko/156.0 Firefox/156.0" },
        { L"firefoxpc", L"Mozilla/5.0 (X11; Linux x86_64; rv:156.0) Gecko/20100101 Firefox/156.0" },
        { L"chrome", L"Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Mobile Safari/537.36" },
        { L"chromepc", L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36" },
    };

    const wchar_t* IdentityAgent(const std::wstring& name)
    {
        for (auto& identity : identities)
        {
            if (name == identity.name)
                return identity.agent;
        }
        return nullptr;
    }

    // Downloads go where the phone's other apps put theirs (WhatsApp, Telegram, Posta): the File app shows them under
    // Download. The phone is rooted, so apps can write there.
    const wchar_t* const downloadFolder = L"C:\\Data\\Users\\Public\\Downloads\\Internet";
    // The top of the page's file list (<input type=file>): the phone's own storage, as the File app shows it.
    const wchar_t* const phoneFolder = L"C:\\Data\\Users\\Public";

    // Tabs with a live page; the others keep only their address until shown again. Each live tab holds its page, its
    // compositor and a swap chain, and the OS gives the app 825 MB.
    const size_t maxLiveTabs = 3;

    // The C API, resolved from webkit.dll.
    struct Engine
    {
        decltype(&lw_init) init;
        decltype(&lw_view_create) viewCreate;
        decltype(&lw_view_resize) viewResize;
        decltype(&lw_view_load_url) loadURL;
        decltype(&lw_view_run_script) runScript;
        decltype(&lw_input_pan) inputPan;
        decltype(&lw_input_tap) inputTap;
        decltype(&lw_input_touch) inputTouch; // optional: tap highlight on press
        decltype(&lw_input_zoom) inputZoom;   // optional: pinch zoom
        decltype(&lw_input_long_press) inputLongPress; // optional: menu on links, text panel
        decltype(&lw_text_edit) textEdit;
        decltype(&lw_text_enter) textEnter;
        decltype(&lw_text_blur) textBlur;
        decltype(&lw_trace_enable) traceEnable;
        decltype(&lw_profile) profile;
        decltype(&lw_gpu_test) gpuTest;
        decltype(&lw_memory_report) memoryReport; // optional: "memoria" in the address box
        decltype(&lw_command) command;            // optional: "bytecode" in the address box
        decltype(&lw_view_reload) reload;
        decltype(&lw_view_clear_site_data) clearSiteData; // optional: page menu
        decltype(&lw_suspend) suspend;
        decltype(&lw_resume) resume;
        decltype(&lw_view_destroy) viewDestroy;
        decltype(&lw_view_set_visible) setVisible;
        decltype(&lw_view_go_back) goBack;
        decltype(&lw_view_go_forward) goForward;
        decltype(&lw_view_stop) stop;
        decltype(&lw_view_find) find;
        decltype(&lw_set_site_user_agent) setSiteUserAgent;
        decltype(&lw_dialog_reply) dialogReply;
        decltype(&lw_file_chooser_reply) fileChooserReply;
        decltype(&lw_picker_reply) pickerReply; // optional: <select>, date, time, colour
        decltype(&lw_password_reply) passwordReply; // optional: HTTP authentication
        decltype(&lw_permission_reply) permissionReply; // optional, as the four below: what pages ask of the phone
        decltype(&lw_geolocation_update) geolocationUpdate;
        decltype(&lw_geolocation_error) geolocationError;
        decltype(&lw_notification_clicked) notificationClicked;
        decltype(&lw_view_exit_fullscreen) exitFullscreen;
        decltype(&lw_download_reply) downloadReply;
        decltype(&lw_view_download) download; // optional: "salva immagine", "scarica link"
        decltype(&lw_window_reply) windowReply; // optional: window.open with its opener
        decltype(&lw_clear_visited_links) clearVisitedLinks; // optional: with the history
        decltype(&lw_network_changed) networkChanged; // optional: Wi-Fi <-> 4G
    };
    Engine engine;

    // The view as the input thread sees it: set once, on the UI thread, by CreateView.
    std::atomic<lw_view*> inputView { nullptr };

    MainPage^ page;
    CoreDispatcher^ dispatcher;
    ULONGLONG startTime;
    ULONGLONG suspendedAt = 0; // GetTickCount64 at the last suspension (site apps lock again after a while away)

    // Log lines from any thread: written at once to LocalState\run.log (so a crash still leaves them on the phone) and
    // taken to the PC by a thread of their own (LogSender). The next launch sends the end of the previous run's file
    // first, and keeps the whole of it as run.prev.log (tools/phonelog reads either).
    std::mutex logLock;
    std::wstring logBuffer;
    HANDLE logFile = INVALID_HANDLE_VALUE;

    std::string Utf8(const std::wstring& text)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n : 0, '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    // ---- the log on the PC (tools/inbox) ----
    // The PC is reachable on the home Wi-Fi only. Nothing that logs ever waits for it: on 4G a blocking connect from the
    // UI thread waited ~21 s for every line, and the app never got past its first one (2026-10-28). On cellular nothing
    // is tried at all (no radio wake-ups for an address that cannot answer); lines that could not go wait, up to 1 MB,
    // and leave when the phone is back on a Wi-Fi where the PC answers.
    enum class PCState { Unknown, Answers, Silent };
    std::atomic<PCState> pcState { PCState::Unknown };
    // The internet goes through the cellular network (NetworkStatusChanged keeps it current).
    std::atomic<bool> onCellular { false };
    std::mutex senderLock;
    std::condition_variable senderWake;
    bool senderPoked = false;   // senderLock: lines to send now
    bool senderRetry = false;   // senderLock: the network changed, try the PC again at once

    // One HTTP POST over a plain socket from the calling thread: the connect gives up after connectLimit ms (a
    // blocking connect to an address that does not answer waits ~21 s). No allocation, so the crash handler can use it.
    // With `commands`, what the PC wants run comes back (tools/phonecmd: the reply's body is "cmd\n" and one line each).
    bool PostToPC(const char* body, size_t length, unsigned connectLimit, std::string* commands = nullptr)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return false;
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(serverPort);
        address.sin_addr.s_addr = htonl((192u << 24) | (168u << 16) | (1u << 8) | 148u); // 192.168.1.148, the PC
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        bool connected = connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval limit = { static_cast<long>(connectLimit / 1000), static_cast<long>(connectLimit % 1000 * 1000) };
            connected = select(0, nullptr, &writable, &failed, &limit) > 0 && FD_ISSET(s, &writable);
        }
        bool sent = false;
        if (connected)
        {
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            DWORD timeout = 2000;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            char header[128];
            int n = sprintf_s(header, "POST /%ls HTTP/1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\n\r\n",
                Site::log, static_cast<unsigned>(length));
            sent = send(s, header, n, 0) == n;
            for (size_t done = 0; sent && done < length;)
            {
                int chunk = send(s, body + done, static_cast<int>(std::min<size_t>(length - done, 64 * 1024)), 0);
                sent = chunk > 0;
                done += sent ? chunk : 0;
            }
            char reply[1024];
            int got = sent ? recv(s, reply, sizeof reply, 0) : 0;
            sent = got > 0;
            if (sent && commands)
            {
                // HTTP/1.0: the reply ends when the PC closes.
                std::string all(reply, got);
                while (all.size() < 64 * 1024 && (got = recv(s, reply, sizeof reply, 0)) > 0)
                    all.append(reply, got);
                size_t at = all.find("\r\n\r\ncmd\n");
                if (at != std::string::npos)
                    *commands = all.substr(at + 8);
            }
        }
        closesocket(s);
        return sent;
    }

    // While the engine starts, every line is written through to the flash and sent at once (a crash then loses
    // nothing); afterwards the file is flushed and the lines sent every 2 s.
    volatile bool sendImmediately = true;

    void PokeSender(bool retry)
    {
        {
            std::lock_guard<std::mutex> lock(senderLock);
            senderPoked = true;
            senderRetry = senderRetry || retry;
        }
        senderWake.notify_one();
    }

    // Commands that came back in the reply to a post nobody could run them from: a line posted by the exception
    // handler (the first-chance C++ exceptions of every start), or any post before the page exists. The PC hands its
    // queue out once, so without this the first commands after a launch were lost. The sender runs them at its next round.
    std::mutex orphanLock;
    std::string orphanCommands;

    // Lines from tools/phonecmd, each run on the UI thread as if typed in the address box.
    void RunCommandsFromPC(const std::string& commands)
    {
        if (!dispatcher || !page)
        {
            std::lock_guard<std::mutex> lock(orphanLock);
            orphanCommands += commands;
            return;
        }
        for (size_t start = 0; start < commands.size();)
        {
            size_t end = commands.find('\n', start);
            if (end == std::string::npos)
                end = commands.size();
            std::string line = commands.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
                continue;
            int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
            std::wstring text(n > 0 ? n : 0, L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), &text[0], n);
            if (!dispatcher || text.empty())
                continue;
            dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([text]()
            {
                if (page)
                    page->RunAddressText(text, true);
            }));
        }
    }

    void LogSender()
    {
        const size_t maxBacklog = 1024 * 1024;
        std::string backlog;
        ULONGLONG retryAt = 0;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(senderLock);
                senderWake.wait_for(lock, std::chrono::seconds(2), [] { return senderPoked; });
                senderPoked = false;
                if (senderRetry)
                    retryAt = 0;
                senderRetry = false;
            }
            if (dispatcher && page)
            {
                std::string orphans;
                {
                    std::lock_guard<std::mutex> lock(orphanLock);
                    orphans.swap(orphanCommands);
                }
                if (!orphans.empty())
                    RunCommandsFromPC(orphans);
            }
            std::wstring text;
            {
                std::lock_guard<std::mutex> lock(logLock);
                text.swap(logBuffer);
            }
            backlog += Utf8(text);
            if (backlog.size() > maxBacklog)
            {
                // The oldest lines go: they are in run.log anyway.
                size_t cut = backlog.find('\n', backlog.size() - maxBacklog);
                backlog.erase(0, cut == std::string::npos ? backlog.size() - maxBacklog : cut + 1);
                backlog.insert(0, "--- righe piu' vecchie non inviate (sono in run.log)\n");
            }
            // With nothing to send, a PC that answers is still asked every 2 s for commands (tools/phonecmd); a
            // suspended app has no running threads, so this stops with it.
            if ((backlog.empty() && pcState != PCState::Answers) || onCellular
                || (pcState == PCState::Silent && GetTickCount64() < retryAt))
            {
                if (!backlog.empty() && onCellular)
                    pcState = PCState::Silent;
                continue;
            }
            std::string commands;
            if (PostToPC(backlog.data(), backlog.size(), 1000, &commands))
            {
                backlog.clear();
                pcState = PCState::Answers;
                RunCommandsFromPC(commands);
            }
            else
            {
                // Another Wi-Fi, or the PC is off: again in a minute, or as soon as the network changes.
                pcState = PCState::Silent;
                retryAt = GetTickCount64() + 60 * 1000;
            }
        }
    }

    void WriteLogFile(const std::string& bytes, bool flush)
    {
        if (logFile == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(logFile, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        // FlushFileBuffers is a flash write. During startup it stays on line by line (a crash before the first timer
        // tick must still leave its lines on disk); afterwards a flush per log line meant a flash write on the Web
        // thread for every keystroke — the exact stall this port exists to avoid. The flush points are FlushLog (2 s)
        // and the crash handler.
        if (flush)
            FlushFileBuffers(logFile);
    }

    void Log(const std::wstring& line)
    {
        wchar_t stamp[24];
        swprintf_s(stamp, L"%6llu ms ", GetTickCount64() - startTime);
        std::wstring text = stamp + line + L"\n";
        std::string bytes = Utf8(text);
        bool immediately = sendImmediately;
        {
            std::lock_guard<std::mutex> lock(logLock);
            WriteLogFile(bytes, immediately);
            logBuffer += text;
        }
        if (immediately)
            PokeSender(false);
    }

    // One suspension's deferral, completed once: by the engine when it has finished (lw_suspend) or by the timer just
    // before the deadline.
    struct Suspension
    {
        Windows::ApplicationModel::SuspendingDeferral^ deferral;
        std::atomic<bool> completed;

        Suspension() : completed(false) { }

        void Complete(const wchar_t* how)
        {
            if (completed.exchange(true))
                return;
            deferral->Complete();
            Log(std::wstring(L"sospensione completata ") + how);
        }
    };

    // Opens run.log. The previous run's file stays on the phone as run.prev.log (a run away from home reached the PC
    // with none of its lines), and its last 64 KB, where a crash is, go to the PC first.
    void OpenLogFile()
    {
        std::wstring folder = ApplicationData::Current->LocalFolder->Path->Data();
        std::wstring path = folder + L"\\run.log";
        CopyFile2(path.c_str(), (folder + L"\\run.prev.log").c_str(), nullptr);
        HANDLE previous = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (previous != INVALID_HANDLE_VALUE)
        {
            const LONGLONG tail = 64 * 1024;
            FILE_STANDARD_INFO info = {};
            GetFileInformationByHandleEx(previous, FileStandardInfo, &info, sizeof info);
            bool cut = info.EndOfFile.QuadPart > tail;
            if (cut)
            {
                LARGE_INTEGER start;
                start.QuadPart = info.EndOfFile.QuadPart - tail;
                SetFilePointerEx(previous, start, nullptr, FILE_BEGIN);
            }
            std::string bytes(static_cast<size_t>(tail), '\0');
            DWORD read = 0;
            ReadFile(previous, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr);
            CloseHandle(previous);
            bytes.resize(read);
            // From the first whole line.
            if (cut)
                bytes.erase(0, std::min(bytes.size(), bytes.find('\n') + 1));
            int n = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring text(n > 0 ? n : 0, L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), &text[0], n);
            logBuffer += std::wstring(L"--- esecuzione precedente") + (cut ? L" (fine; tutta in run.prev.log)" : L"") + L"\n"
                + text + L"--- fine esecuzione precedente\n";
        }
        logFile = CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);

        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        try
        {
            auto profile = Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
            onCellular = profile && profile->IsWwanConnectionProfile;
        }
        catch (Exception^)
        {
        }
        Windows::System::Threading::ThreadPool::RunAsync(ref new Windows::System::Threading::WorkItemHandler(
            [](IAsyncAction^) { LogSender(); }), Windows::System::Threading::WorkItemPriority::Low);
    }

    // ---- the phone's connection: Wi-Fi, cellular, none ----
    std::mutex networkLock;
    bool networkKnown = false;   // networkLock
    std::wstring networkKey;     // networkLock: the profile (SSID, operator) and adapter the internet goes through

    // At start, when the engine is up (engineStarted), and on every NetworkStatusChanged (any thread; it also fires for
    // changes that are not a new network). The engine hears whether there is a connection and whether it is another
    // network now (the connections and WebSockets opened on the old one are dead); the log sender tries the PC again at
    // once (back home).
    void CheckNetwork(bool engineStarted)
    {
        std::wstring key;
        std::wstring description = L"nessuna";
        bool online = false;
        bool cellular = false;
        try
        {
            auto profile = Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
            if (profile)
            {
                online = true;
                cellular = profile->IsWwanConnectionProfile;
                std::wstring name = profile->ProfileName ? profile->ProfileName->Data() : L"";
                key = name;
                if (profile->NetworkAdapter)
                    key += std::wstring(L"|") + profile->NetworkAdapter->NetworkAdapterId.ToString()->Data();
                description = std::wstring(profile->IsWlanConnectionProfile ? L"Wi-Fi" : cellular ? L"cellulare" : L"altra")
                    + L" \"" + name + L"\"";
            }
        }
        catch (Exception^)
        {
            return;
        }
        bool changed;
        bool switched;
        {
            std::lock_guard<std::mutex> lock(networkLock);
            changed = !networkKnown || key != networkKey;
            switched = networkKnown && key != networkKey;
            networkKnown = true;
            networkKey = key;
        }
        if (!changed && !engineStarted)
            return;
        onCellular = cellular;
        if (changed)
            Log(L"rete: " + description + (switched ? L" (cambiata)" : L""));
        if (engine.networkChanged)
            engine.networkChanged(online ? 1 : 0, switched ? 1 : 0);
        if (changed)
            PokeSender(true);
    }

    // Module bases for the crash report: the offset inside webkit.dll is looked up on the PC (engine/out symbols).
    HMODULE engineModule;
    HMODULE eglModule;
    HMODULE glesModule;

    void AppendModule(char* out, size_t size, const char* label, uintptr_t address)
    {
        struct { const char* name; HMODULE module; } modules[] = {
            { "webkit.dll", engineModule }, { "libEGL.dll", eglModule }, { "libGLESv2.dll", glesModule },
        };
        char part[96];
        sprintf_s(part, " %s=0x%08X", label, static_cast<unsigned>(address));
        strcat_s(out, size, part);
        for (const auto& m : modules)
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(m.module);
            // Our DLLs are well under 64 MB.
            if (base && address >= base && address < base + 64 * 1024 * 1024)
            {
                sprintf_s(part, " (%s+0x%X)", m.name, static_cast<unsigned>(address - base));
                strcat_s(out, size, part);
            }
        }
    }

    // In KERNELBASE on the phone, but hidden from app code by the WP8.1 SDK headers.
    extern "C" __declspec(dllimport) PVOID WINAPI AddVectoredExceptionHandler(ULONG first, LONG (CALLBACK* handler)(EXCEPTION_POINTERS*));

    // First-chance view of every exception: errors (0xC..., 0x8...) are written to run.log before the process dies.
    // C++ exceptions, thread naming and debug prints are skipped. No allocation: the heap may be what failed.
    LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
    {
        DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == 0x406D1388 || (code & 0xF0000000) == 0x40000000)
            return EXCEPTION_CONTINUE_SEARCH;
        // C++ exceptions (WinRT errors included) are usually caught, but an uncaught one ends in a fail-fast that
        // never reaches this handler: the first few are logged so the throw that killed the app is in run.log.
        static volatile LONG cppReported;
        if (code == 0xE06D7363 && InterlockedIncrement(&cppReported) > 6)
            return EXCEPTION_CONTINUE_SEARCH;
        static volatile LONG reported;
        if (InterlockedIncrement(&reported) > 8)
            return EXCEPTION_CONTINUE_SEARCH;
        char line[512];
        sprintf_s(line, "%6llu ms ECCEZIONE 0x%08X thread %u", GetTickCount64() - startTime, static_cast<unsigned>(code),
            static_cast<unsigned>(GetCurrentThreadId()));
        AppendModule(line, sizeof line, "pc", reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress));
        AppendModule(line, sizeof line, "lr", info->ContextRecord->Lr);
        // Flags and Thumb IT state at the fault: "illegal instruction" on an ordinary branch after an IT block means the
        // IT bits were wrong (0xC000001D in RenderLayerBacking::positionOverflowControlsLayers, 2026-10-28).
        {
            char status[32];
            sprintf_s(status, " cpsr=0x%08X", static_cast<unsigned>(info->ContextRecord->Cpsr));
            strcat_s(line, status);
        }
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        {
            char part[64];
            sprintf_s(part, " %s 0x%08X", info->ExceptionRecord->ExceptionInformation[0] ? "scrittura" : "lettura",
                static_cast<unsigned>(info->ExceptionRecord->ExceptionInformation[1]));
            strcat_s(line, part);
        }
        strcat_s(line, "\n");
        if (logFile != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(logFile, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
            FlushFileBuffers(logFile);
        }
        // At once, from this thread: the process may be gone before the sender's next round. Only where the PC has
        // been answering (at home); elsewhere the line is in run.log for the next launch.
        if (pcState == PCState::Answers)
        {
            std::string commands;
            PostToPC(line, strlen(line), 300, &commands);
            if (!commands.empty())
            {
                std::lock_guard<std::mutex> lock(orphanLock);
                orphanCommands += commands;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::wstring Text(const wchar_t* text)
    {
        return text ? std::wstring(text) : std::wstring();
    }

    // Main-frame loads that ended, for the "banco" bench (it runs on the UI thread, the event on an engine thread).
    std::atomic<int> g_loadsEnded{ 0 };

    // Engine threads: copy what the event carries (valid only during the call), log it, hand UI changes to the UI thread.
    void __cdecl OnEngineEvent(void*, lw_view* view, const lw_event* event)
    {
        std::wstring text = Text(event->text);
        std::wstring text2 = Text(event->text2);
        int type = event->type;
        long long number = event->number;

        switch (type)
        {
        case LW_EVENT_TITLE: Log(L"titolo: " + text); break;
        case LW_EVENT_URL: Log(L"url: " + text); break;
        case LW_EVENT_LOAD_STATE:
        {
            static const wchar_t* const states[] = { L"?", L"inizio", L"commit", L"fine", L"errore" };
            Log(std::wstring(L"caricamento: ") + states[number >= 1 && number <= 4 ? number : 0]
                + (text.empty() ? L"" : L" " + text + L" " + text2));
            if (number == LW_LOAD_FINISHED || number == LW_LOAD_FAILED)
                g_loadsEnded++;
            break;
        }
        case LW_EVENT_CONSOLE: Log(L"console[" + std::to_wstring(number) + L"] " + text + L" (" + text2 + L")"); break;
        case LW_EVENT_NEW_WINDOW: Log(std::wstring(number == 1 ? L"window.open: " : L"nuova scheda da link: ") + text); break;
        case LW_EVENT_CLOSE_WINDOW: Log(L"window.close()"); break;
        case LW_EVENT_HISTORY: Log(L"cronologia della scheda: indietro " + std::to_wstring(number & 1) + L", avanti "
            + std::to_wstring((number >> 1) & 1)); break;
        case LW_EVENT_DIALOG: Log(L"dialogo " + std::to_wstring(number) + L" (" + std::to_wstring(text.size()) + L" caratteri)"); break;
        case LW_EVENT_FIND_RESULT: Log(L"trova: " + std::to_wstring(number) + L" risultati"); break;
        case LW_EVENT_DOWNLOAD: Log(L"download " + std::to_wstring(event->id) + L": " + (text2.empty() ? L"(nessun file)" : text2)); break;
        case LW_EVENT_DOWNLOAD_PROGRESS: break;
        case LW_EVENT_DOWNLOAD_FINISHED: Log(L"download " + std::to_wstring(event->id) + L" finito: " + std::to_wstring(number) + L" " + text2); break;
        case LW_EVENT_FILE_CHOOSER: Log(L"scelta file " + std::to_wstring(event->id) + L": " + text); break;
        case LW_EVENT_PICKER: Log(L"selettore " + std::to_wstring(event->id) + L": tipo " + std::to_wstring(number)); break;
        case LW_EVENT_PASSWORD: Log(L"password " + std::to_wstring(event->id) + L": " + text + L", rifiuti " + std::to_wstring(number)); break;
        case LW_EVENT_PERMISSION: Log(L"permesso " + std::to_wstring(event->id) + L": tipo " + std::to_wstring(number) + L", " + text); break;
        case LW_EVENT_GEOLOCATION: Log(L"posizione: sorgente " + std::to_wstring(number)); break;
        case LW_EVENT_NOTIFICATION: Log(L"notifica " + std::to_wstring(event->id) + (number ? L": mostra" : L": chiudi")); break;
        case LW_EVENT_SHARE: Log(L"condividi dalla pagina: " + std::to_wstring(text.size()) + L" caratteri, indirizzo " + std::to_wstring(text2.size())); break;
        case LW_EVENT_FULLSCREEN: Log(std::wstring(L"schermo intero: ") + (number ? L"si" : L"no")); break;
        case LW_EVENT_CLIPBOARD: Log(L"appunti: la pagina ha copiato " + std::to_wstring(text.size()) + L" caratteri"); break;
        case LW_EVENT_PROGRESS: break;
        case LW_EVENT_TRACE: Log(L"motore: " + text); break;
        case LW_EVENT_FIELD_FOCUS:
        case LW_EVENT_FIELD_UPDATE:
        {
            // The field struct and its string live only during the callback: copy both here, on the engine thread, and
            // let the UI thread decide what to draw. Nothing in this handler may block (the Web thread is behind it).
            FieldState state;
            if (const lw_field* field = event->field)
            {
                state.kind = field->kind;
                state.selectionStart = field->selection_start;
                state.selectionEnd = field->selection_end;
                state.x = field->x;
                state.y = field->y;
                state.width = field->width;
                state.height = field->height;
                state.enterAction = field->enter_action;
                state.maxLength = field->max_length;
                state.sequence = field->sequence;
                state.text = Text(field->text);
            }
            if (type == LW_EVENT_FIELD_FOCUS)
                Log(std::wstring(L"campo UI: kind ") + std::to_wstring(state.kind) + L", "
                    + std::to_wstring(state.text.size()) + L" caratteri, seq " + std::to_wstring(state.sequence));
            // FOCUS and UPDATE are not the same thing: an update may only refresh a bar that is already open for that
            // same field. Treating an update as a focus (what the first build did) reopened the bar right after the
            // user closed the keyboard, and the two fought each other (see the barra aperta/chiusa loop in the log).
            bool update = type == LW_EVENT_FIELD_UPDATE;
            // Fields of a tab that is not shown have no bar.
            if (view != inputView.load())
                return;
            dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([state, update]()
            {
                page->OnFieldEventOnUIThread(state, update);
            }));
            return;
        }
        case LW_EVENT_CONTEXT_MENU:
        {
            std::wstring paragraph = event->field ? Text(event->field->text) : std::wstring();
            Log(L"menu: link " + std::to_wstring(text.size()) + L", immagine " + std::to_wstring(text2.size()) + L", testo "
                + std::to_wstring(paragraph.size()));
            if (view != inputView.load())
                return;
            dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([text, text2, paragraph]()
            {
                page->OnContextMenuOnUIThread(text, text2, paragraph);
            }));
            return;
        }
        case LW_EVENT_SCRIPT_RESULT:
            if (event->id == 0x1C0) // the page's icons, for its tile (PinToStart)
            {
                dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([text]()
                {
                    page->OnSiteIconLinks(text);
                }));
                return;
            }
            Log(L"script " + std::to_wstring(event->id) + (number ? L" ERRORE: " : L": ") + text);
            break;
        default: Log(L"evento " + std::to_wstring(type) + L" " + text); break;
        }

        // The first page is in: from now on the log goes in 2 s batches.
        if (type == LW_EVENT_LOAD_STATE && (number == LW_LOAD_FINISHED || number == LW_LOAD_FAILED))
            sendImmediately = false;

        switch (type)
        {
        case LW_EVENT_URL: case LW_EVENT_LOAD_STATE: case LW_EVENT_PROGRESS: case LW_EVENT_TITLE: case LW_EVENT_HISTORY:
        case LW_EVENT_NEW_WINDOW: case LW_EVENT_CLOSE_WINDOW: case LW_EVENT_DIALOG: case LW_EVENT_FIND_RESULT:
        case LW_EVENT_DOWNLOAD: case LW_EVENT_DOWNLOAD_PROGRESS: case LW_EVENT_DOWNLOAD_FINISHED: case LW_EVENT_FILE_CHOOSER:
        case LW_EVENT_PICKER: case LW_EVENT_PASSWORD: case LW_EVENT_PERMISSION: case LW_EVENT_GEOLOCATION:
        case LW_EVENT_NOTIFICATION: case LW_EVENT_SHARE: case LW_EVENT_FULLSCREEN: case LW_EVENT_CLIPBOARD:
            break;
        default:
            return;
        }
        int id = event->id;
        dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([view, type, number, id, text, text2]()
        {
            page->OnEngineEventOnUIThread(view, type, number, id, text, text2);
        }));
    }
}

MainPage::MainPage()
    : m_view(nullptr)
    , m_textSequence(0)
    , m_syncField(false)
    , m_composeVisible(false)
    , m_barKind(LW_FIELD_NONE)
    , m_tracing(false)
    , m_iconGeneration(0)
    , m_siteIcons(0)
    , m_benchStep(0)
    , m_benchLoads(0)
    , m_benchAt(0)
    , m_benchResting(false)
    , m_awake(false)
    , m_selectAddressOnKeyboard(false)
    , m_sheetIsLink(false)
    , m_shareIsLink(false)
    , m_nextTabId(1)
    , m_viewWidth(0)
    , m_viewHeight(0)
    , m_viewScale(1)
    , m_historyDirty(false)
    , m_listKind(ListKind::None)
    , m_dialogView(nullptr)
    , m_dialogId(0)
    , m_dialogHasInput(false)
    , m_dialogReplaced(false)
    , m_fullscreenView(nullptr)
    , m_findId(0)
    , m_findMatches(0)
    , m_findIndex(0)
    , m_findDirection(0)
    , m_downloadsLoaded(false)
    , m_downloadsRefreshed(0)
    , m_pickView(nullptr)
    , m_pickId(0)
    , m_pickMultiple(false)
    , m_pickerView(nullptr)
    , m_pickerId(0)
    , m_unlocked(false)
    , m_lockBusy(false)
    , m_lockSetup(false)
    , m_lockStep(3)
    , m_changingPin(false)
    , m_lockFailures(0)
    , m_lockWaitUntil(0)
{
    // The log is opened before the page's XAML is parsed: if the markup (or this constructor) is what kills the app,
    // run.log still says so, and the next launch sends it to the PC.
    startTime = GetTickCount64();
    OpenLogFile();
    AddVectoredExceptionHandler(1, OnException);
    CheckNetwork(false);
    Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged +=
        ref new Windows::Networking::Connectivity::NetworkStatusChangedEventHandler([](Object^) { CheckNetwork(false); });

    Log(L"--- MainPage: prima di InitializeComponent");
    try
    {
        InitializeComponent();
    }
    catch (Exception^ e)
    {
        // A markup error ends the app with a black screen: its message goes to the log first.
        Log(L"InitializeComponent: errore 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)) + L" " + e->Message->Data());
        throw;
    }
    Log(L"--- MainPage: InitializeComponent ok");
    page = this;
    dispatcher = Dispatcher;
    LoadEntries();
    if (Site::app)
        ApplySiteMode();
    // The last run's copies: photos picked for pages and files handed to other apps.
    auto removeFolder = [](StorageFolder^ parent, String^ name)
    {
        create_task(parent->GetFolderAsync(name)).then([](StorageFolder^ old)
        {
            return create_task(old->DeleteAsync());
        }).then([](task<void> deleted)
        {
            try { deleted.get(); } catch (Exception^) { }
        });
    };
    removeFolder(ApplicationData::Current->TemporaryFolder, L"upload");
    removeFolder(ApplicationData::Current->LocalFolder, L"Open");

    // Suspension: the session is saved for the next start, and the engine hides the page, writes localStorage to disk
    // and releases what it can (the process may not come back). The deferral keeps the app running until the engine is
    // done, and is completed here half a second before the deadline in any case, so a busy page never gets the app
    // ended. Both calls only post to the engine's threads.
    Windows::UI::Xaml::Application::Current->Suspending += ref new SuspendingEventHandler([](Object^, Windows::ApplicationModel::SuspendingEventArgs^ args)
    {
        suspendedAt = GetTickCount64();
        if (page)
        {
            page->SaveSession();
            page->SaveHistory();
        }
        if (!engine.suspend)
            return;
        auto suspension = std::make_shared<Suspension>();
        suspension->deferral = args->SuspendingOperation->GetDeferral();
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        long long left = args->SuspendingOperation->Deadline.UniversalTime - ((static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime);
        TimeSpan delay;
        delay.Duration = std::max(left - 5000000LL, 0LL);
        Windows::System::Threading::ThreadPoolTimer::CreateTimer(ref new Windows::System::Threading::TimerElapsedHandler(
            [suspension](Windows::System::Threading::ThreadPoolTimer^) { suspension->Complete(L"alla scadenza"); }), delay);
        // The engine's reference goes with its callback.
        engine.suspend([](void* context)
        {
            auto* suspension = static_cast<std::shared_ptr<Suspension>*>(context);
            (*suspension)->Complete(L"dal motore");
            delete suspension;
        }, new std::shared_ptr<Suspension>(suspension));
    });
    Windows::UI::Xaml::Application::Current->Resuming += ref new EventHandler<Object^>([](Object^, Object^)
    {
        ApplicationData::Current->LocalSettings->Values->Insert(L"sessionSaved", PropertyValue::CreateBoolean(false));
        if (engine.resume)
            engine.resume();
        unsigned long long away = GetTickCount64() - suspendedAt;
        if (page && dispatcher)
            dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([away]() { page->OnSiteResumed(away); }));
    });
    Windows::ApplicationModel::DataTransfer::DataTransferManager::GetForCurrentView()->DataRequested += ref new TypedEventHandler<
        Windows::ApplicationModel::DataTransfer::DataTransferManager^, Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^>(
            this, &MainPage::ShareRequested);
    Windows::Phone::UI::Input::HardwareButtons::BackPressed += ref new EventHandler<
        Windows::Phone::UI::Input::BackPressedEventArgs^>(this, &MainPage::HardwareBack);

    m_logTimer = ref new DispatcherTimer();
    TimeSpan interval;
    interval.Duration = 2 * 10000000LL;
    m_logTimer->Interval = interval;
    m_logTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::FlushLog);
    m_logTimer->Start();

    // The compose bar has to sit above the keyboard, so it follows the input pane instead of guessing its height.
    m_inputPane = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
    if (m_inputPane)
    {
        m_inputShowingToken = m_inputPane->Showing += ref new TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
                this, &MainPage::InputPane_Showing);
        m_inputHidingToken = m_inputPane->Hiding += ref new TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
                this, &MainPage::InputPane_Hiding);
    }
}

// The engine (a 56 MB DLL and its start) loads on a background thread: the UI never waits for it.
void MainPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    Log(L"--- avvio");
    // Launched from a page's tile on Start: its address (App::OnLaunched).
    if (auto argument = dynamic_cast<String^>(e->Parameter))
    {
        if (argument->Length())
            m_pendingLaunchURL = argument->Data();
    }
    create_task([this]() { return StartEngine(); }).then([this](bool started)
    {
        if (!started)
        {
            this->SetLoadLine(true, true);
            return;
        }
        // this->: inside a lambda VS2013 reads a bare Panel as the XAML class.
        this->Pages->SizeChanged += ref new SizeChangedEventHandler(this, &MainPage::Panel_SizeChanged);
        if (this->Pages->ActualWidth > 0 && this->Pages->ActualHeight > 0)
            CreateView(static_cast<int>(this->Pages->ActualWidth), static_cast<int>(this->Pages->ActualHeight));
    }, task_continuation_context::use_current());
}

bool MainPage::StartEngine()
{
    Log(L"carico webkit.dll");
    HMODULE dll = LoadPackagedLibrary(L"webkit.dll", 0);
    if (!dll)
    {
        Log(L"LoadPackagedLibrary(webkit.dll) fallita, errore " + std::to_wstring(GetLastError()));
        return false;
    }
    engineModule = dll;
    eglModule = LoadPackagedLibrary(L"libEGL.dll", 0);
    glesModule = LoadPackagedLibrary(L"libGLESv2.dll", 0);
    wchar_t bases[160];
    swprintf_s(bases, L"basi: webkit.dll 0x%08X libEGL 0x%08X libGLESv2 0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(dll)),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(eglModule)), static_cast<unsigned>(reinterpret_cast<uintptr_t>(glesModule)));
    Log(bases);
    engine.init = reinterpret_cast<decltype(engine.init)>(GetProcAddress(dll, "lw_init"));
    engine.viewCreate = reinterpret_cast<decltype(engine.viewCreate)>(GetProcAddress(dll, "lw_view_create"));
    engine.viewResize = reinterpret_cast<decltype(engine.viewResize)>(GetProcAddress(dll, "lw_view_resize"));
    engine.loadURL = reinterpret_cast<decltype(engine.loadURL)>(GetProcAddress(dll, "lw_view_load_url"));
    engine.runScript = reinterpret_cast<decltype(engine.runScript)>(GetProcAddress(dll, "lw_view_run_script"));
    engine.inputPan = reinterpret_cast<decltype(engine.inputPan)>(GetProcAddress(dll, "lw_input_pan"));
    engine.inputTap = reinterpret_cast<decltype(engine.inputTap)>(GetProcAddress(dll, "lw_input_tap"));
    engine.inputTouch = reinterpret_cast<decltype(engine.inputTouch)>(GetProcAddress(dll, "lw_input_touch"));
    engine.inputZoom = reinterpret_cast<decltype(engine.inputZoom)>(GetProcAddress(dll, "lw_input_zoom"));
    engine.inputLongPress = reinterpret_cast<decltype(engine.inputLongPress)>(GetProcAddress(dll, "lw_input_long_press"));
    engine.textEdit = reinterpret_cast<decltype(engine.textEdit)>(GetProcAddress(dll, "lw_text_edit"));
    engine.textEnter = reinterpret_cast<decltype(engine.textEnter)>(GetProcAddress(dll, "lw_text_enter"));
    engine.textBlur = reinterpret_cast<decltype(engine.textBlur)>(GetProcAddress(dll, "lw_text_blur"));
    engine.traceEnable = reinterpret_cast<decltype(engine.traceEnable)>(GetProcAddress(dll, "lw_trace_enable"));
    engine.profile = reinterpret_cast<decltype(engine.profile)>(GetProcAddress(dll, "lw_profile"));
    engine.gpuTest = reinterpret_cast<decltype(engine.gpuTest)>(GetProcAddress(dll, "lw_gpu_test"));
    engine.memoryReport = reinterpret_cast<decltype(engine.memoryReport)>(GetProcAddress(dll, "lw_memory_report"));
    engine.command = reinterpret_cast<decltype(engine.command)>(GetProcAddress(dll, "lw_command"));
    engine.reload = reinterpret_cast<decltype(engine.reload)>(GetProcAddress(dll, "lw_view_reload"));
    engine.clearSiteData = reinterpret_cast<decltype(engine.clearSiteData)>(GetProcAddress(dll, "lw_view_clear_site_data"));
    engine.suspend = reinterpret_cast<decltype(engine.suspend)>(GetProcAddress(dll, "lw_suspend"));
    engine.resume = reinterpret_cast<decltype(engine.resume)>(GetProcAddress(dll, "lw_resume"));
    engine.viewDestroy = reinterpret_cast<decltype(engine.viewDestroy)>(GetProcAddress(dll, "lw_view_destroy"));
    engine.setVisible = reinterpret_cast<decltype(engine.setVisible)>(GetProcAddress(dll, "lw_view_set_visible"));
    engine.goBack = reinterpret_cast<decltype(engine.goBack)>(GetProcAddress(dll, "lw_view_go_back"));
    engine.goForward = reinterpret_cast<decltype(engine.goForward)>(GetProcAddress(dll, "lw_view_go_forward"));
    engine.stop = reinterpret_cast<decltype(engine.stop)>(GetProcAddress(dll, "lw_view_stop"));
    engine.find = reinterpret_cast<decltype(engine.find)>(GetProcAddress(dll, "lw_view_find"));
    engine.setSiteUserAgent = reinterpret_cast<decltype(engine.setSiteUserAgent)>(GetProcAddress(dll, "lw_set_site_user_agent"));
    engine.dialogReply = reinterpret_cast<decltype(engine.dialogReply)>(GetProcAddress(dll, "lw_dialog_reply"));
    engine.fileChooserReply = reinterpret_cast<decltype(engine.fileChooserReply)>(GetProcAddress(dll, "lw_file_chooser_reply"));
    engine.pickerReply = reinterpret_cast<decltype(engine.pickerReply)>(GetProcAddress(dll, "lw_picker_reply"));
    engine.passwordReply = reinterpret_cast<decltype(engine.passwordReply)>(GetProcAddress(dll, "lw_password_reply"));
    engine.permissionReply = reinterpret_cast<decltype(engine.permissionReply)>(GetProcAddress(dll, "lw_permission_reply"));
    engine.geolocationUpdate = reinterpret_cast<decltype(engine.geolocationUpdate)>(GetProcAddress(dll, "lw_geolocation_update"));
    engine.geolocationError = reinterpret_cast<decltype(engine.geolocationError)>(GetProcAddress(dll, "lw_geolocation_error"));
    engine.notificationClicked = reinterpret_cast<decltype(engine.notificationClicked)>(GetProcAddress(dll, "lw_notification_clicked"));
    engine.exitFullscreen = reinterpret_cast<decltype(engine.exitFullscreen)>(GetProcAddress(dll, "lw_view_exit_fullscreen"));
    engine.downloadReply = reinterpret_cast<decltype(engine.downloadReply)>(GetProcAddress(dll, "lw_download_reply"));
    engine.download = reinterpret_cast<decltype(engine.download)>(GetProcAddress(dll, "lw_view_download"));
    engine.windowReply = reinterpret_cast<decltype(engine.windowReply)>(GetProcAddress(dll, "lw_window_reply"));
    engine.clearVisitedLinks = reinterpret_cast<decltype(engine.clearVisitedLinks)>(GetProcAddress(dll, "lw_clear_visited_links"));
    engine.networkChanged = reinterpret_cast<decltype(engine.networkChanged)>(GetProcAddress(dll, "lw_network_changed"));
    if (!engine.init || !engine.viewCreate || !engine.viewResize || !engine.loadURL || !engine.viewDestroy
        || !engine.setVisible || !engine.goBack || !engine.goForward || !engine.stop || !engine.dialogReply
        || !engine.inputPan || !engine.inputTap || !engine.textEdit || !engine.textEnter || !engine.textBlur)
    {
        Log(L"webkit.dll senza le funzioni attese");
        return false;
    }
    Log(L"webkit.dll caricata");

    // Kept alive for the life of the app: the engine copies them anyway.
    static std::wstring resources = Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data();
    static std::wstring data = ApplicationData::Current->LocalFolder->Path->Data();
    static std::wstring cache = ApplicationData::Current->LocalCacheFolder->Path->Data();

    lw_config config = {};
    config.resource_directory = resources.c_str();
    config.data_directory = data.c_str();
    config.cache_directory = cache.c_str();
    config.memory_limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
    config.user_agent = mobileUserAgent;
    config.download_directory = downloadFolder;
    // A site app keeps its login across the system ending it in the background (PLAN §64.1).
    config.keep_session_cookies = Site::app ? 1 : 0;
    config.callback = OnEngineEvent;
    Log(L"lw_init...");
    int status = engine.init(&config);
    Log(L"lw_init " + std::to_wstring(status));
    // The engine starts believing it is online: tell it how things are.
    if (status == 0)
        CheckNetwork(true);
    if (status == 0 && engine.setSiteUserAgent)
    {
        for (auto host : alwaysDesktopSites)
            engine.setSiteUserAgent(host, desktopUserAgent);
        for (auto& host : m_desktopSites)
            engine.setSiteUserAgent(host.c_str(), desktopUserAgent);
        for (auto& site : m_siteIdentities)
        {
            if (auto agent = IdentityAgent(site.second))
                engine.setSiteUserAgent(site.first.c_str(), agent);
        }
    }
    // Development: the tracer starts on; "traccia" in the address box turns it off and on.
    if (status == 0 && engine.traceEnable)
    {
        engine.traceEnable(1);
        m_tracing = true;
        Log(L"tracer acceso");
    }
    return status == 0;
}

// The view is created at the panel's first real size, in view pixels; the scale maps them to physical pixels.
void MainPage::Panel_SizeChanged(Object^, SizeChangedEventArgs^ e)
{
    CreateView(static_cast<int>(e->NewSize.Width), static_cast<int>(e->NewSize.Height));
}

void MainPage::CreateView(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    bool first = m_viewWidth == 0;
    m_viewWidth = width;
    m_viewHeight = height;
    m_viewScale = static_cast<float>(DisplayInformation::GetForCurrentView()->RawPixelsPerViewPixel);
    if (!first)
    {
        // Every live tab follows the panel's size (the hidden ones too: no relayout when they come back).
        for (auto& tab : m_tabs)
        {
            if (tab->view)
                engine.viewResize(tab->view, width, height);
        }
        return;
    }
    RestoreSession();
    if (!Site::app && m_tabs.empty() && m_pendingLaunchURL.empty() && pcState != PCState::Answers)
    {
        // Away from home the test pages (on the PC) cannot load: an empty tab with the favorites.
        NewTab(L"about:blank", 0, true);
        ShowList(ListKind::Favorites);
    }
    else if (m_tabs.empty())
        NewTab(m_pendingLaunchURL.empty() ? (Site::app ? Site::start : startPage) : m_pendingLaunchURL, 0, true);
    else if (!m_pendingLaunchURL.empty())
        NewTab(m_pendingLaunchURL, 0, true);
    m_pendingLaunchURL.clear();
}

// Saved at every suspension: every tab's address, one per line, and which one was shown.
void MainPage::SaveSession()
{
    std::wstring urls;
    int active = 0;
    for (size_t i = 0; i < m_tabs.size() && i < 20; ++i)
    {
        if (m_tabs[i] == m_activeTab)
            active = static_cast<int>(i);
        std::wstring url = m_tabs[i]->url;
        if (urls.size() + url.size() + 1 > 7000)
            break;
        urls += (urls.empty() ? L"" : L"\n") + url;
    }
    auto values = ApplicationData::Current->LocalSettings->Values;
    values->Insert(L"tabs", ref new String(urls.c_str()));
    values->Insert(L"activeTab", PropertyValue::CreateInt32(active));
    values->Insert(L"sessionSaved", PropertyValue::CreateBoolean(true));
}

// The tabs of the last run come back when it ended suspended (the system closed the app in the background, or the
// user did from the task list): not after a crash, so a page that ends the app is not loaded again at every start.
// Only the tab that was shown loads; the others wait as addresses until they are opened.
void MainPage::RestoreSession()
{
    auto values = ApplicationData::Current->LocalSettings->Values;
    bool saved = values->HasKey(L"sessionSaved") && safe_cast<bool>(values->Lookup(L"sessionSaved"));
    values->Insert(L"sessionSaved", PropertyValue::CreateBoolean(false));
    if (!saved || !values->HasKey(L"tabs"))
    {
        Log(L"sessione: niente da riaprire (l'ultima esecuzione non e' finita sospesa)");
        return;
    }
    std::wstring urls = safe_cast<String^>(values->Lookup(L"tabs"))->Data();
    int active = values->HasKey(L"activeTab") ? safe_cast<int>(values->Lookup(L"activeTab")) : 0;
    std::vector<std::wstring> list;
    for (size_t start = 0; start < urls.size();)
    {
        size_t end = std::min(urls.find(L'\n', start), urls.size());
        if (end > start)
            list.push_back(urls.substr(start, end - start));
        start = end + 1;
    }
    if (list.empty())
        return;
    if (active < 0 || active >= static_cast<int>(list.size()))
        active = 0;
    for (size_t i = 0; i < list.size(); ++i)
    {
        auto tab = std::make_shared<Tab>();
        tab->id = m_nextTabId++;
        tab->url = list[i];
        tab->title = list[i];
        m_tabs.push_back(tab);
    }
    Log(L"sessione: " + std::to_wstring(list.size()) + L" schede, riapro " + list[active]);
    ActivateTab(m_tabs[active]);
}

// ---- touch ----
// One finger: a move beyond 8 px pans (the engine applies it on its scrolling thread), anything shorter is a tap (press
// + release on the Web thread's input lane). The engine calls only post work, so the thread that delivers the pointer
// events never waits for the page.
//
// That thread is our own (plan: "input thread"): the SwapChainPanel's CoreIndependentInputSource delivers touch on a
// high-priority thread-pool thread that does nothing else, so a busy XAML thread (compose bar, keyboard, layout, the
// log upload) cannot delay a finger. The XAML pointer events on the UI thread are only the fallback.

namespace
{
    // The thread that delivers the pointer events (input thread, or UI thread in the fallback): the long-press timer
    // brings its timeout back there, so the tracker stays single-threaded.
    Windows::UI::Core::CoreDispatcher^ touchDispatcher;

    struct TouchTracker
    {
        bool tracking = false;
        bool panning = false;
        unsigned int pointerId = 0;
        Windows::Foundation::Point press;
        Windows::Foundation::Point last;
        unsigned long long pressTimestamp = 0;
        // Second finger: a pinch, which replaces the pan or tap of the first. When either finger lifts the gesture is
        // over; the other finger is ignored until it lifts too.
        bool zooming = false;
        unsigned int secondId = 0;
        Windows::Foundation::Point second;
        float zoomStartDistance = 0;
        unsigned int remainingId = 0;
        // Long press: the finger held 500 ms without panning or a second finger. Then neither tap nor pan follows.
        unsigned int pressGeneration = 0;
        bool longPressed = false;

        void StartLongPressTimer()
        {
            unsigned int generation = ++pressGeneration;
            longPressed = false;
            if (!engine.inputLongPress || !touchDispatcher)
                return;
            TimeSpan delay;
            delay.Duration = 500 * 10000LL;
            Windows::System::Threading::ThreadPoolTimer::CreateTimer(ref new Windows::System::Threading::TimerElapsedHandler(
                [this, generation](Windows::System::Threading::ThreadPoolTimer^)
            {
                touchDispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([this, generation]()
                {
                    LongPressTimeout(generation);
                }));
            }), delay);
        }

        void LongPressTimeout(unsigned int generation)
        {
            lw_view* view = inputView.load();
            if (generation != pressGeneration || !tracking || panning || zooming || !view)
                return;
            longPressed = true;
            engine.inputLongPress(view, press.X, press.Y);
        }

        static float Distance(Windows::Foundation::Point a, Windows::Foundation::Point b)
        {
            float dx = a.X - b.X;
            float dy = a.Y - b.Y;
            return sqrtf(dx * dx + dy * dy);
        }

        static Windows::Foundation::Point Center(Windows::Foundation::Point a, Windows::Foundation::Point b)
        {
            return Windows::Foundation::Point((a.X + b.X) / 2, (a.Y + b.Y) / 2);
        }

        bool StartZoom(Windows::UI::Input::PointerPoint^ point)
        {
            lw_view* view = inputView.load();
            if (!view || !engine.inputZoom || zooming)
                return false;
            float distance = Distance(last, point->Position);
            if (distance < 20.0f)
                return false;
            if (longPressed)
                return false;
            ++pressGeneration;
            if (panning)
                engine.inputPan(view, LW_PHASE_CANCEL, last.X, last.Y, 0, 0, 0);
            panning = false;
            zooming = true;
            secondId = point->PointerId;
            second = point->Position;
            zoomStartDistance = distance;
            // Both fingers go to the page too (a map or a drawing zooms by itself): the engine ignores the zoom where
            // the page keeps the gesture.
            if (engine.inputTouch)
                engine.inputTouch(view, secondId, LW_PHASE_BEGIN, second.X, second.Y, point->Timestamp);
            Windows::Foundation::Point center = Center(last, second);
            engine.inputZoom(view, LW_PHASE_BEGIN, 1.0f, center.X, center.Y);
            return true;
        }

        // True when the press starts a gesture: the caller captures the pointer.
        bool Pressed(Windows::UI::Input::PointerPoint^ point)
        {
            lw_view* view = inputView.load();
            if (tracking && view && point->PointerId != pointerId)
                return StartZoom(point);
            if (!view || tracking)
                return false;
            tracking = true;
            zooming = false;
            panning = false;
            pointerId = point->PointerId;
            press = point->Position;
            last = press;
            pressTimestamp = point->Timestamp;
            StartLongPressTimer();
            // The engine highlights what is under the finger shortly after, unless a pan starts first.
            if (engine.inputTouch)
                engine.inputTouch(view, pointerId, LW_PHASE_BEGIN, press.X, press.Y, pressTimestamp);
            static bool first = true;
            if (first)
            {
                first = false;
                Log(L"primo tocco a " + std::to_wstring(static_cast<int>(press.X)) + L","
                    + std::to_wstring(static_cast<int>(press.Y)) + L" tipo "
                    + std::to_wstring(static_cast<int>(point->PointerDevice->PointerDeviceType)));
            }
            return true;
        }

        void Moved(Windows::UI::Input::PointerPoint^ point)
        {
            lw_view* view = inputView.load();
            if (zooming)
            {
                if (!view)
                    return;
                if (!tracking)
                {
                    // After the pinch: the finger still down goes on to the page only.
                    if (point->PointerId == remainingId && engine.inputTouch)
                        engine.inputTouch(view, remainingId, LW_PHASE_UPDATE, point->Position.X, point->Position.Y, point->Timestamp);
                    return;
                }
                if (point->PointerId != pointerId && point->PointerId != secondId)
                    return;
                if (point->PointerId == pointerId)
                    last = point->Position;
                else
                    second = point->Position;
                if (engine.inputTouch)
                    engine.inputTouch(view, point->PointerId, LW_PHASE_UPDATE, point->Position.X, point->Position.Y, point->Timestamp);
                Windows::Foundation::Point center = Center(last, second);
                engine.inputZoom(view, LW_PHASE_UPDATE, Distance(last, second) / zoomStartDistance, center.X, center.Y);
                return;
            }
            if (!tracking || !view || point->PointerId != pointerId || longPressed)
                return;
            Windows::Foundation::Point position = point->Position;
            // The page sees every move (touchmove, pointermove), before the pan: a page that takes the gesture (a
            // slider, a captcha, a map) stops the scroll.
            if (engine.inputTouch)
                engine.inputTouch(view, pointerId, LW_PHASE_UPDATE, position.X, position.Y, point->Timestamp);
            float totalX = position.X - press.X;
            float totalY = position.Y - press.Y;
            // A finger that stays within a few pixels is a tap candidate: the pan starts only once it really moves, so
            // a tap does not scroll the page by one or two pixels first.
            if (!panning && totalX * totalX + totalY * totalY > 64.0f)
            {
                panning = true;
                ++pressGeneration;
                engine.inputPan(view, LW_PHASE_BEGIN, press.X, press.Y, 0, 0, pressTimestamp);
            }
            if (panning)
                engine.inputPan(view, LW_PHASE_UPDATE, position.X, position.Y, position.X - last.X, position.Y - last.Y,
                    point->Timestamp);
            last = position;
        }

        // True when the release ends the gesture: the caller releases the capture.
        bool Released(Windows::UI::Input::PointerPoint^ point)
        {
            lw_view* view = inputView.load();
            if (zooming && (point->PointerId == pointerId || point->PointerId == secondId))
            {
                // The first finger up ends the pinch; the other one only goes to the page until it lifts too.
                if (tracking)
                {
                    if (view && engine.inputTouch)
                        engine.inputTouch(view, point->PointerId, LW_PHASE_END, point->Position.X, point->Position.Y, point->Timestamp);
                    if (view)
                        engine.inputZoom(view, LW_PHASE_END, 0, 0, 0);
                    tracking = false;
                    remainingId = point->PointerId == pointerId ? secondId : pointerId;
                }
                else if (point->PointerId == remainingId)
                {
                    if (view && engine.inputTouch)
                        engine.inputTouch(view, remainingId, LW_PHASE_END, point->Position.X, point->Position.Y, point->Timestamp);
                    zooming = false;
                }
                return true;
            }
            if (!tracking || point->PointerId != pointerId)
                return false;
            tracking = false;
            ++pressGeneration;
            if (view && engine.inputTouch)
                engine.inputTouch(view, pointerId, LW_PHASE_END, point->Position.X, point->Position.Y, point->Timestamp);
            if (longPressed)
            {
                // The menu took the gesture: no tap.
                longPressed = false;
                return true;
            }
            static bool first = true;
            if (first)
            {
                first = false;
                Log(panning ? L"primo rilascio: pan" : L"primo rilascio: tocco");
            }
            if (view && panning)
                engine.inputPan(view, LW_PHASE_END, point->Position.X, point->Position.Y, 0, 0, point->Timestamp);
            else if (view)
                engine.inputTap(view, point->Position.X, point->Position.Y, point->Timestamp);
            panning = false;
            return true;
        }

        void Canceled()
        {
            lw_view* view = inputView.load();
            if (zooming)
            {
                if (tracking && view)
                    engine.inputZoom(view, LW_PHASE_CANCEL, 0, 0, 0);
                if (view && engine.inputTouch)
                {
                    if (tracking)
                    {
                        engine.inputTouch(view, pointerId, LW_PHASE_CANCEL, last.X, last.Y, 0);
                        engine.inputTouch(view, secondId, LW_PHASE_CANCEL, second.X, second.Y, 0);
                    }
                    else
                        engine.inputTouch(view, remainingId, LW_PHASE_CANCEL, 0, 0, 0);
                }
                tracking = false;
                zooming = false;
                return;
            }
            if (!tracking)
                return;
            tracking = false;
            ++pressGeneration;
            longPressed = false;
            if (panning && view)
                engine.inputPan(view, LW_PHASE_CANCEL, last.X, last.Y, 0, 0, 0);
            if (view && engine.inputTouch)
                engine.inputTouch(view, pointerId, LW_PHASE_CANCEL, last.X, last.Y, 0);
            panning = false;
        }
    };

    // Touched by one thread only: the input thread, or the UI thread when the fallback is in use.
    TouchTracker touch;
}

// Each tab's panel has its own independent input source on a thread of its own: only the visible panel gets fingers,
// so the tracker still sees one gesture at a time. (A transparent input panel over all the tabs would need a Background,
// which a SwapChainPanel does not take: 0.1.0.13 failed to load its page.)
void MainPage::StartInput(SwapChainPanel^ panel)
{
    auto work = ref new Windows::System::Threading::WorkItemHandler([this, panel](IAsyncAction^)
    {
        CoreIndependentInputSource^ source;
        try
        {
            source = panel->CreateCoreIndependentInputSource(
                CoreInputDeviceTypes::Touch | CoreInputDeviceTypes::Pen | CoreInputDeviceTypes::Mouse);
        }
        catch (Exception^ e)
        {
            Log(L"input: CreateCoreIndependentInputSource fallita (0x" + std::to_wstring(static_cast<unsigned>(e->HResult))
                + L"), uso il thread UI");
            dispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([this, panel]() { this->StartXamlInput(panel); }));
            return;
        }
        source->PointerPressed += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &MainPage::Input_PointerPressed);
        source->PointerMoved += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &MainPage::Input_PointerMoved);
        source->PointerReleased += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &MainPage::Input_PointerReleased);
        source->PointerCaptureLost += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &MainPage::Input_PointerCaptureLost);
        // The UI thread keeps it with the tab: to stop this loop when the tab goes, and to bring the long-press timer here.
        dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, panel, source]()
        {
            for (auto& tab : m_tabs)
            {
                if (tab->panel != panel)
                    continue;
                tab->input = source;
                if (tab == m_activeTab)
                    touchDispatcher = source->Dispatcher;
            }
        }));
        Log(L"input: thread indipendente pronto");
        // Runs until the tab's panel goes (Discard): every pointer event of the panel is dispatched on this thread.
        source->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    });
    Windows::System::Threading::ThreadPool::RunAsync(work, Windows::System::Threading::WorkItemPriority::High,
        Windows::System::Threading::WorkItemOptions::TimeSliced);
}

// An exception out of a handler would go back to the input dispatcher as a failure and end the app in a fail-fast:
// logged and swallowed instead, so the finger just does nothing.
static void InputError(const wchar_t* where, int result)
{
    static int reported;
    if (reported++ >= 8)
        return;
    wchar_t code[16];
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(result));
    Log(std::wstring(L"input: errore in ") + where + L" " + code);
}

void MainPage::Input_PointerPressed(Object^ sender, PointerEventArgs^ e)
{
    try
    {
        auto source = dynamic_cast<CoreIndependentInputSource^>(sender);
        if (touch.Pressed(e->CurrentPoint) && source)
            source->SetPointerCapture();
    }
    catch (Exception^ error)
    {
        InputError(L"Input_PointerPressed", error->HResult);
    }
    e->Handled = true;
}

void MainPage::Input_PointerMoved(Object^, PointerEventArgs^ e)
{
    try
    {
        touch.Moved(e->CurrentPoint);
    }
    catch (Exception^ error)
    {
        InputError(L"Input_PointerMoved", error->HResult);
    }
    e->Handled = true;
}

void MainPage::Input_PointerReleased(Object^, PointerEventArgs^ e)
{
    try
    {
        // No ReleasePointerCapture: on WP8.1 it throws 0x8007007F (not implemented) on this source, and lifting the
        // finger ends the capture anyway.
        touch.Released(e->CurrentPoint);
    }
    catch (Exception^ error)
    {
        InputError(L"Input_PointerReleased", error->HResult);
    }
    e->Handled = true;
}

void MainPage::Input_PointerCaptureLost(Object^, PointerEventArgs^ e)
{
    try
    {
        touch.Canceled();
    }
    catch (Exception^ error)
    {
        InputError(L"Input_PointerCaptureLost", error->HResult);
    }
    e->Handled = true;
}

// Fallback: the same gestures from XAML's pointer events on the tab's panel, on the UI thread.
void MainPage::StartXamlInput(SwapChainPanel^ panel)
{
    touchDispatcher = Dispatcher;
    panel->PointerPressed += ref new PointerEventHandler(this, &MainPage::Panel_PointerPressed);
    panel->PointerMoved += ref new PointerEventHandler(this, &MainPage::Panel_PointerMoved);
    panel->PointerReleased += ref new PointerEventHandler(this, &MainPage::Panel_PointerReleased);
    panel->PointerCanceled += ref new PointerEventHandler(this, &MainPage::Panel_PointerCanceled);
    panel->PointerCaptureLost += ref new PointerEventHandler(this, &MainPage::Panel_PointerCanceled);
}

void MainPage::Panel_PointerPressed(Object^ sender, PointerRoutedEventArgs^ e)
{
    auto panel = safe_cast<UIElement^>(sender);
    if (touch.Pressed(e->GetCurrentPoint(panel)))
        panel->CapturePointer(e->Pointer);
    e->Handled = true;
}

void MainPage::Panel_PointerMoved(Object^ sender, PointerRoutedEventArgs^ e)
{
    touch.Moved(e->GetCurrentPoint(safe_cast<UIElement^>(sender)));
    e->Handled = true;
}

void MainPage::Panel_PointerReleased(Object^ sender, PointerRoutedEventArgs^ e)
{
    auto panel = safe_cast<UIElement^>(sender);
    if (touch.Released(e->GetCurrentPoint(panel)))
        panel->ReleasePointerCapture(e->Pointer);
    e->Handled = true;
}

void MainPage::Panel_PointerCanceled(Object^, PointerRoutedEventArgs^ e)
{
    touch.Canceled();
    e->Handled = true;
}

// What the address box means: an address as typed (https unless it says otherwise), or a Google search. An address
// has no spaces and a dot or a port (example.com, 192.168.1.148:8765, localhost:8080); anything else is a search.
// Local addresses (an IPv4 address, localhost) default to http, since the PC's test server has no TLS.
static std::wstring AddressToURL(const std::wstring& typed)
{
    size_t first = typed.find_first_not_of(L" \t");
    size_t last = typed.find_last_not_of(L" \t");
    if (first == std::wstring::npos)
        return std::wstring();
    std::wstring text = typed.substr(first, last - first + 1);
    if (text.find(L"://") != std::wstring::npos || text.compare(0, 6, L"about:") == 0 || text.compare(0, 5, L"data:") == 0)
        return text;
    bool hasSpace = text.find(L' ') != std::wstring::npos;
    size_t hostEnd = text.find_first_of(L"/?#");
    std::wstring host = text.substr(0, hostEnd);
    size_t colon = host.find(L':');
    std::wstring hostName = host.substr(0, colon);
    bool hasPort = colon != std::wstring::npos && colon + 1 < host.size()
        && host.find_first_not_of(L"0123456789", colon + 1) == std::wstring::npos;
    bool hasDot = hostName.find(L'.') != std::wstring::npos && hostName.back() != L'.' && hostName.front() != L'.';
    bool isLocalhost = hostName == L"localhost";
    if (hasSpace || hostName.empty() || (!hasDot && !hasPort && !isLocalhost))
        return std::wstring(L"https://www.google.com/search?q=") + Windows::Foundation::Uri::EscapeComponent(ref new String(text.c_str()))->Data();
    bool isIPv4 = hostName.find_first_not_of(L"0123456789.") == std::wstring::npos;
    return (isIPv4 || isLocalhost ? L"http://" : L"https://") + text;
}

void MainPage::Navigate(const std::wstring& text)
{
    auto tab = ActiveTab();
    if (!tab || text.empty())
        return;
    std::wstring url = AddressToURL(text);
    if (url.empty())
        return;
    Address->Text = ref new String(url.c_str());
    Log(L"vai a " + url);
    // The bar shows where the tab is going from now on, as browsers do while the page loads.
    tab->url = url;
    m_currentURL = url;
    if (!tab->view)
    {
        // A discarded tab loads its new address as it comes back.
        EnsureLive(tab);
        return;
    }
    engine.loadURL(tab->view, url.c_str());
}

// "banco": the same pages, twice, in the active tab: each one loads (up to 90 s), then stays at rest for 10 s, which is
// what shows a page that keeps working after loading. The first round after a change of engine compiles and fills the
// bytecode cache; the second reads it and profiles the rest. Log lines "banco: <round> <address>", read by tools/loads.py --banco, which
// compares two runs. The screen stays on: the bench is meant to run alone, on the charger.
void MainPage::BenchStart()
{
    if (m_benchTimer && m_benchTimer->IsEnabled)
    {
        m_benchTimer->Stop();
        m_benchDisplay->RequestRelease();
        Log(L"banco: fermato");
        return;
    }
    if (!m_benchTimer)
    {
        m_benchTimer = ref new DispatcherTimer();
        TimeSpan interval;
        interval.Duration = 5000000LL;
        m_benchTimer->Interval = interval;
        m_benchTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::BenchTick);
        m_benchDisplay = ref new Windows::System::Display::DisplayRequest();
    }
    m_benchDisplay->RequestActive();
    m_benchStep = -1;
    m_benchAt = 0;
    m_benchResting = true;
    m_benchTimer->Start();
    Log(L"banco: inizio");
}

void MainPage::BenchTick(Object^, Object^)
{
    static const wchar_t* const pages[] = {
        L"http://192.168.1.148:8765/browser/anim.html",
        L"https://en.wikipedia.org/wiki/Italy",
        L"https://mail.google.com/mail/mu/",
        L"https://www.poste.it/",
        L"https://m.youtube.com/",
        L"https://gemini.google.com/app",
    };
    const int count = sizeof(pages) / sizeof(pages[0]);
    const int rounds = 2;
    ULONGLONG now = GetTickCount64();
    if (!m_benchResting)
    {
        if (g_loadsEnded.load() == m_benchLoads && now - m_benchAt < 90000)
            return;
        m_benchResting = true;  // loaded, or given up on: the rest starts
        m_benchAt = now;
        // The second round also samples the rest: what a page does when nobody touches it (tools/profile.py --all).
        if (m_benchStep >= count && engine.profile)
            engine.profile(8000);
        if (engine.command)
            engine.command("scope");
        return;
    }
    if (m_benchAt && now - m_benchAt < 10000)
        return;
    if (++m_benchStep >= count * rounds)
    {
        m_benchTimer->Stop();
        m_benchDisplay->RequestRelease();
        if (engine.memoryReport)
            engine.memoryReport();
        Log(L"banco: fine");
        return;
    }
    Log(L"banco: " + std::to_wstring(m_benchStep / count + 1) + L" " + pages[m_benchStep % count]);
    m_benchLoads = g_loadsEnded.load();
    m_benchResting = false;
    m_benchAt = now;
    Navigate(pages[m_benchStep % count]);
}

// Tapping the address selects all of it, as in IE: typing replaces it at once. Deferred, because the tap itself places
// the caret after the focus arrives and would undo an immediate selection.
void MainPage::Address_GotFocus(Object^, RoutedEventArgs^)
{
    // Editing the address: it takes the whole bar but for its clear button, and the matching favorites and history
    // rise over the page.
    ClearAddress->Visibility = Windows::UI::Xaml::Visibility::Visible;
    Favorites->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Reload->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    TabsButton->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    More->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Suggest->Visibility = Windows::UI::Xaml::Visibility::Visible;
    UpdateSuggestions();
    // The keyboard opening moves the caret again (and drops the selection): select once more when it is up.
    m_selectAddressOnKeyboard = true;
    Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([this]() { Address->SelectAll(); }));
}

void MainPage::Address_LostFocus(Object^, RoutedEventArgs^)
{
    ClearAddress->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Favorites->Visibility = Windows::UI::Xaml::Visibility::Visible;
    Reload->Visibility = Windows::UI::Xaml::Visibility::Visible;
    TabsButton->Visibility = Windows::UI::Xaml::Visibility::Visible;
    More->Visibility = Windows::UI::Xaml::Visibility::Visible;
    Suggest->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    BarShift->Y = 0;
    Suggest->Margin = Thickness(0);
    m_selectAddressOnKeyboard = false;
    // What was typed and not sent gives way to the page's address again.
    Address->Text = ref new String(m_currentURL.c_str());
}

void MainPage::Address_TextChanged(Object^, TextChangedEventArgs^)
{
    if (Suggest->Visibility == Windows::UI::Xaml::Visibility::Visible)
        UpdateSuggestions();
}

void MainPage::ClearAddress_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    Address->Text = L"";
}

void MainPage::SetLoadLine(bool visible, bool failed)
{
    LoadLine->Visibility = visible ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    if (failed)
        LoadScale->ScaleX = 1;
    LoadLine->Fill = ref new SolidColorBrush(failed ? Windows::UI::ColorHelper::FromArgb(255, 0xE5, 0x39, 0x35)
                                                    : Windows::UI::ColorHelper::FromArgb(255, 0x19, 0x76, 0xD2));
}

// The page's menu: the same sheet as the long press, with the current address as its title.
void MainPage::More_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    auto tab = ActiveTab();
    if (!tab)
        return;
    MenuItems->Children->Clear();
    MenuTitle->Text = ref new String(m_currentURL.c_str());
    if (tab->canGoForward && tab->view)
        AddMenuItem(L"avanti", [this]() { if (m_view) engine.goForward(m_view); });
    if (Site::app)
    {
        // A site app: the page, its login and nothing of the browser.
        std::wstring here = m_currentURL;
        AddMenuItem(L"pagina iniziale", [this]() { if (m_view) engine.loadURL(m_view, Site::start); });
        AddMenuItem(L"trova nella pagina", [this]() { ShowFind(); });
        if (m_unlocked)
        {
            AddMenuItem(L"compila l'accesso", [this]() { if (auto t = ActiveTab()) FillLogin(t->view, t->url); });
            if (NoPin())
                AddMenuItem(L"proteggi con un PIN", [this]()
                {
                    m_changingPin = true;
                    Lock->Visibility = Windows::UI::Xaml::Visibility::Visible;
                    ShowLockStep(1, nullptr);
                });
            else
            {
                AddMenuItem(L"blocca", [this]() { ShowLock(L"Bloccata."); });
                AddMenuItem(L"apri senza PIN", [this]() { SetupVault(noPinSecret); });
            }
        }
        AddMenuItem(L"cambia credenziali", [this]() { ForgetCredentials(); });
        AddMenuItem(L"apri in Internet", [here]()
        {
            create_task(Windows::System::Launcher::LaunchUriAsync(ref new Uri(ref new String((L"internet:u/" + here).c_str()))))
                .then([](task<bool> t) { try { t.get(); } catch (Exception^) { } });
        });
        AddMenuItem(L"download", [this]() { ShowList(ListKind::Downloads); });
        if (engine.clearSiteData)
            AddMenuItem(L"esci e cancella i dati del sito", [this]() { if (m_view) engine.clearSiteData(m_view); });
        Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
        return;
    }
    std::wstring url = m_currentURL;
    std::wstring title = tab->title;
    bool web = url.compare(0, 7, L"http://") == 0 || url.compare(0, 8, L"https://") == 0;
    if (web)
    {
        AddMenuItem(IsFavorite(url) ? L"togli dai preferiti" : L"aggiungi ai preferiti", [this, url, title]() { ToggleFavorite(url, title); });
        AddMenuItem(L"trova nella pagina", [this]() { ShowFind(); });
        AddMenuItem(L"condividi pagina", [this, url]() { Share(url, true); });
        // The site's own layout for phones, or the one for computers (and back), then the page again.
        AddMenuItem(IsDesktopSite(url) ? L"versione mobile del sito" : L"versione desktop del sito", [this, url]() { ToggleDesktopSite(url); });
        AddMenuItem(Windows::UI::StartScreen::SecondaryTile::Exists(ref new String(PinTileId(url).c_str())) ? L"cambia icona su Start" : L"aggiungi a Start",
            [this, url, title]() { PinToStart(url, title); });
        // A site our engine cannot handle yet: the same address in the phone's own browser.
        AddMenuItem(L"apri in Internet Explorer", [url]()
        {
            create_task(Windows::System::Launcher::LaunchUriAsync(ref new Uri(ref new String(url.c_str())))).then([](task<bool> t)
            {
                try { t.get(); } catch (Exception^) { }
            });
        });
    }
    AddMenuItem(L"nuova scheda", [this]() { NewTab(L"about:blank", 0, true); Address->Focus(Windows::UI::Xaml::FocusState::Programmatic); });
    AddMenuItem(L"download", [this]() { ShowList(ListKind::Downloads); });
    // Cookies, localStorage, IndexedDB and cache of this site, then the page again from the network: a site stuck on
    // a bad login or a broken cached state starts over as on a first visit.
    if (web && engine.clearSiteData)
        AddMenuItem(L"cancella dati del sito e ricarica", [this]() { if (m_view) engine.clearSiteData(m_view); });
    Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::Favorites_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    ShowList(ListKind::Favorites);
}

// Reload, or stop a page that is still loading.
void MainPage::Reload_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    auto tab = ActiveTab();
    if (!tab)
        return;
    if (!tab->view)
    {
        EnsureLive(tab);
        return;
    }
    if (tab->loading)
        engine.stop(tab->view);
    else if (engine.reload)
        engine.reload(tab->view);
}

void MainPage::TabsButton_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    ShowList(ListKind::Tabs);
}

// The folder above `folder` in the page's file list: the phone's storage is the top, and a memory card's root goes
// back to it.
static std::wstring ParentFolder(const std::wstring& folder)
{
    if (_wcsicmp(folder.c_str(), phoneFolder) == 0 || folder.size() <= 3)
        return phoneFolder;
    size_t slash = folder.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return phoneFolder;
    // D:\Music -> D:\ (the card's root)
    return slash == 2 ? folder.substr(0, 3) : folder.substr(0, slash);
}

// The phone's Back key closes what is open over the page first (a dialog is cancelled), then goes back in the tab's
// own history; on a tab's first page it returns to the tab that opened it, and otherwise leaves the app.
void MainPage::HardwareBack(Object^, Windows::Phone::UI::Input::BackPressedEventArgs^ e)
{
    // The PIN screen: Back leaves the app (not handled).
    if (Lock->Visibility == Windows::UI::Xaml::Visibility::Visible)
        return;
    if (DialogPanel->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        e->Handled = true;
        CloseDialog(false);
        return;
    }
    if (IconPicker->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        e->Handled = true;
        CloseIconPicker();
        return;
    }
    if (Sheet->Visibility == Windows::UI::Xaml::Visibility::Visible || Menu->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        e->Handled = true;
        Sheet->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        Menu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        return;
    }
    if (ListPanel->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        e->Handled = true;
        // The page's file list climbs to the parent folder; above the phone's storage it closes (no file).
        if (m_listKind == ListKind::Files && _wcsicmp(m_pickFolder.c_str(), phoneFolder) != 0)
        {
            ShowFolder(ParentFolder(m_pickFolder));
            return;
        }
        HideList();
        return;
    }
    if (FindBar->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        e->Handled = true;
        HideFind();
        return;
    }
    // A page element is fullscreen: Back ends that, the page stays.
    if (m_fullscreenView)
    {
        e->Handled = true;
        if (engine.exitFullscreen)
            engine.exitFullscreen(m_fullscreenView);
        else
            ShowFullscreen(nullptr);
        return;
    }
    auto tab = ActiveTab();
    if (!tab)
        return;
    if (tab->canGoBack && tab->view)
    {
        e->Handled = true;
        engine.goBack(tab->view);
        return;
    }
    if (tab->openerId)
    {
        for (auto& other : m_tabs)
        {
            if (other->id == tab->openerId)
            {
                e->Handled = true;
                auto opener = other;
                CloseTab(tab);
                ActivateTab(opener);
                return;
            }
        }
    }
}

void MainPage::Address_KeyUp(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter)
        return;
    e->Handled = true;
    RunAddressText(Address->Text->Data(), false);
}

// What the address box does with its text: a command for the engine or the app, or somewhere to go. fromPC = the text
// came from tools/phonecmd, not from the keyboard.
// A command from the PC (tools/phonecmd, tools/banco) keeps the screen on for five minutes more: a bench driven from the
// PC runs with nobody touching the phone, and a phone that locks suspends the app in the middle of it.
void MainPage::StayAwakeForPC()
{
    if (!m_awakeTimer)
    {
        m_awakeTimer = ref new DispatcherTimer();
        TimeSpan interval;
        interval.Duration = 5 * 60 * 10000000LL;
        m_awakeTimer->Interval = interval;
        m_awakeTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::StayAwakeTick);
        m_awakeDisplay = ref new Windows::System::Display::DisplayRequest();
    }
    if (!m_awake)
    {
        m_awakeDisplay->RequestActive();
        m_awake = true;
    }
    m_awakeTimer->Stop();
    m_awakeTimer->Start();
}

void MainPage::StayAwakeTick(Object^, Object^)
{
    m_awakeTimer->Stop();
    if (m_awake)
    {
        m_awakeDisplay->RequestRelease();
        m_awake = false;
    }
}

void MainPage::RunAddressText(std::wstring text, bool fromPC)
{
    if (fromPC)
        StayAwakeForPC();
    if (fromPC)
        Log(L"dal PC: " + text);
    // "profilo" or "profilo <seconds>": the engine samples the Web thread and logs the profile (tools/profile.py).
    // The commands below are typed on the phone's keyboard, which capitalizes the first letter and adds a space after a
    // suggested word: they are matched trimmed and in lower case.
    {
        std::wstring command = text;
        command.erase(0, command.find_first_not_of(L" \t"));
        command.erase(command.find_last_not_of(L" \t") + 1);
        std::transform(command.begin(), command.end(), command.begin(), towlower);
        if (command.compare(0, 7, L"profilo") == 0 || command == L"memoria" || command == L"traccia" || command == L"gpu"
            || command == L"ua" || command.compare(0, 3, L"ua ") == 0 || command == L"bytecode" || command == L"ic" || command == L"banco" || command.compare(0, 7, L"motore ") == 0)
            text = command;
    }
    // "banco": the phase 5 bench (BenchTick); typed again, it stops.
    if (text == L"banco")
    {
        BenchStart();
        HideKeyboard();
        return;
    }
    // "motore <command>": anything for the engine's lw_command (its answer is a log line).
    if (text.compare(0, 7, L"motore ") == 0 && engine.command)
    {
        engine.command(Utf8(text.substr(7)).c_str());
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    // "bytecode": the engine's bytecode cache off or on again, to compare a page with and without it (engine log).
    if ((text == L"bytecode" || text == L"ic") && engine.command)
    {
        engine.command(text == L"ic" ? "ic" : "bytecode");
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    // "ua <name>": the browser this site sees from now on (identities above); "ua" alone says which it is.
    if (text == L"ua" || text.compare(0, 3, L"ua ") == 0)
    {
        std::wstring name = text.size() > 3 ? text.substr(3) : std::wstring();
        name.erase(0, name.find_first_not_of(L" "));
        SetSiteIdentity(m_currentURL, name);
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    if (text.compare(0, 7, L"profilo") == 0 && engine.profile)
    {
        int seconds = _wtoi(text.c_str() + 7);
        engine.profile((seconds > 0 ? seconds : 10) * 1000);
        Log(L"profilo richiesto");
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    // "memoria": where the process's memory is, by owner and by address-space allocation (engine log).
    if (text == L"memoria" && engine.memoryReport)
    {
        engine.memoryReport();
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    // "traccia": the engine's latency tracer (Web thread activity, slow frames) off or on again.
    if (text == L"traccia" && engine.traceEnable)
    {
        m_tracing = !m_tracing;
        engine.traceEnable(m_tracing ? 1 : 0);
        Log(m_tracing ? L"tracer acceso" : L"tracer spento");
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    // "gpu": a 6 s frame timeline while the user scrolls (engine log, tools/timeline.py).
    if (text == L"gpu" && engine.gpuTest)
    {
        engine.gpuTest();
        Log(L"prova gpu richiesta");
        Address->Text = ref new String(m_currentURL.c_str());
        HideKeyboard();
        return;
    }
    Navigate(text);
    HideKeyboard();
}

// The keyboard closes when the address box loses the focus. A Page takes the focus only with IsTabStop (false by
// default: the old Focus() call on the page failed silently and the keyboard stayed up); disabling the box for an
// instant is the fallback that always drops it.
void MainPage::HideKeyboard()
{
    IsTabStop = true;
    if (Focus(Windows::UI::Xaml::FocusState::Programmatic) && Address->FocusState == Windows::UI::Xaml::FocusState::Unfocused)
        return;
    Address->IsEnabled = false;
    Address->IsEnabled = true;
}

void MainPage::OnEngineEventOnUIThread(lw_view* view, int type, long long number, int id, const std::wstring& text,
                                       const std::wstring& text2)
{
    // Downloads outlive their tab.
    if (type == LW_EVENT_DOWNLOAD || type == LW_EVENT_DOWNLOAD_PROGRESS || type == LW_EVENT_DOWNLOAD_FINISHED)
    {
        OnDownloadEvent(type, number, id, text, text2);
        return;
    }
    // The phone has one position source, whatever tab asked (and that tab may be gone when it says stop).
    if (type == LW_EVENT_GEOLOCATION)
    {
        WatchPosition(static_cast<int>(number));
        return;
    }
    auto tab = TabForView(view);
    if (!tab)
        return;
    bool active = tab == m_activeTab;
    switch (type)
    {
    case LW_EVENT_PROGRESS:
        tab->progress = static_cast<int>(number);
        // The load line grows with the page's progress (a render transform: no layout pass).
        if (active)
            LoadScale->ScaleX = static_cast<double>(std::max(number, 5LL)) / 100;
        return;
    case LW_EVENT_URL:
        tab->url = text;
        if (active)
        {
            m_currentURL = text;
            if (Address->FocusState == Windows::UI::Xaml::FocusState::Unfocused)
                Address->Text = ref new String(text.c_str());
        }
        return;
    case LW_EVENT_TITLE:
        tab->title = text;
        if (Site::app && active)
            SiteTitle->Text = ref new String((text.empty() ? Site::name : text).c_str());
        AddHistory(tab->url, text);
        return;
    case LW_EVENT_HISTORY:
        tab->canGoBack = (number & 1) != 0;
        tab->canGoForward = (number & 2) != 0;
        return;
    case LW_EVENT_NEW_WINDOW:
        // window.open: a tab whose page the engine makes at once, for the opener to talk to (sign-in popups).
        if (number == 1)
        {
            OpenWindow(view, id, text);
            return;
        }
        // A link with target=_blank: a new tab in front, which Back on its first page closes. A site app sends links
        // to other sites to Internet.
        if (Site::app && !IsSiteURL(text) && (text.compare(0, 7, L"http://") == 0 || text.compare(0, 8, L"https://") == 0))
        {
            create_task(Windows::System::Launcher::LaunchUriAsync(ref new Uri(ref new String((L"internet:u/" + text).c_str()))))
                .then([](task<bool> t) { try { t.get(); } catch (Exception^) { } });
            return;
        }
        NewTab(text, tab->id, true);
        return;
    case LW_EVENT_CLOSE_WINDOW:
    {
        // window.close(): the tab goes, and the one that opened it comes back.
        int openerId = tab->openerId;
        CloseTab(tab);
        for (auto& other : m_tabs)
        {
            if (active && other->id == openerId)
            {
                ActivateTab(other);
                break;
            }
        }
        return;
    }
    case LW_EVENT_DIALOG:
        ShowPageDialog(view, id, static_cast<int>(number), text, text2);
        return;
    case LW_EVENT_FILE_CHOOSER:
        PickFiles(view, id, number != 0, text);
        return;
    case LW_EVENT_PICKER:
        ShowPicker(view, id, static_cast<int>(number), text, text2);
        return;
    case LW_EVENT_PASSWORD:
        ShowPasswordQuestion(view, id, static_cast<int>(number), text, text2);
        return;
    case LW_EVENT_PERMISSION:
        ShowPermissionQuestion(view, id, static_cast<int>(number), text);
        return;
    case LW_EVENT_NOTIFICATION:
        ShowPageNotification(view, id, number != 0, text, text2);
        return;
    case LW_EVENT_SHARE:
        // navigator.share: an address alone goes as a link, words (with the address after them) as text.
        if (active && !(text.empty() && text2.empty()))
        {
            bool link = text.empty();
            Share(link ? text2 : text2.empty() ? text : text + L"\n" + text2, link);
        }
        return;
    case LW_EVENT_CLIPBOARD:
        // A page copied text (a "copy" button): it is on the engine's clipboard, for the pages of the app. For the rest
        // of the phone the text opens in the panel, where the phone's own selection copies it.
        if (active && !text.empty())
        {
            std::wstring copied = text;
            ShowNotice(L"Copiato per le pagine di Internet. Tocca per copiarlo anche per le altre app.", [this, copied]() { ShowSheet(copied, false); });
        }
        return;
    case LW_EVENT_FULLSCREEN:
        if (number && active)
            ShowFullscreen(view);
        else if (number)
        {
            // A tab nobody sees does not take the screen.
            if (engine.exitFullscreen)
                engine.exitFullscreen(view);
        }
        else if (view == m_fullscreenView)
            ShowFullscreen(nullptr);
        return;
    case LW_EVENT_FIND_RESULT:
        if (id != m_findId || !active)
            return;
        m_findMatches = static_cast<int>(number);
        if (!m_findMatches)
            m_findIndex = 0;
        else if (m_findDirection == 0)
            m_findIndex = 1;
        else
            m_findIndex = (m_findIndex - 1 + m_findDirection + m_findMatches) % m_findMatches + 1;
        FindCount->Text = m_findMatches ? ref new String((std::to_wstring(m_findIndex) + L" di " + std::to_wstring(m_findMatches)).c_str())
                                        : (FindText->Text->Length() ? L"nessuno" : L"");
        return;
    case LW_EVENT_LOAD_STATE:
        break;
    default:
        return;
    }
    if (number == LW_LOAD_COMMITTED)
        AddHistory(tab->url, std::wstring());
    tab->loading = number == LW_LOAD_STARTED || number == LW_LOAD_COMMITTED;
    tab->failed = number == LW_LOAD_FAILED;
    if (active)
        UpdateBar();
    if (Site::app && number == LW_LOAD_FINISHED && tab->view)
        FillLogin(tab->view, tab->url);
}

// Every 2 s on the UI thread: run.log to the flash and the memory checks. The lines go to the PC from LogSender.
void MainPage::FlushLog(Object^, Object^)
{
    // The flush is outside the log lock: FlushFileBuffers can take milliseconds on a phone, and the Web thread must not
    // be behind that lock while it types.
    // The OS ends an app over its memory limit without any exception: a line whenever usage moves by 16 MB tells
    // that apart from a crash.
    static unsigned long long loggedMemory;
    unsigned long long memory = Windows::System::MemoryManager::AppMemoryUsage;
    if (memory > loggedMemory + (16ull << 20) || memory + (16ull << 20) < loggedMemory)
    {
        loggedMemory = memory;
        Log(L"memoria: " + std::to_wstring(memory >> 20) + L" MB di "
            + std::to_wstring(Windows::System::MemoryManager::AppMemoryUsageLimit >> 20) + L" MB, livello "
            + std::to_wstring(static_cast<int>(Windows::System::MemoryManager::AppMemoryUsageLevel)));
    }
    // Near the limit the hidden tabs go first (4g): each holds a whole page. The shown one stays.
    if (Windows::System::MemoryManager::AppMemoryUsageLevel == Windows::System::AppMemoryUsageLevel::High)
        DiscardBeyond(1);
    if (logFile != INVALID_HANDLE_VALUE)
        FlushFileBuffers(logFile);
}

// ---- compose bar (phase 3.d) ----
// Rule this section follows: the UI thread renders what the finger does — the character appears in the bar, typed by
// the system keyboard — and only *sends* the resulting edit. The engine applies it on the Web thread's input lane.
// Nothing here waits for the Web thread, and no engine call happens per frame.

namespace
{
    // Text and caret of whichever control is showing. PasswordBox is a separate control on WP8.1 (TextBox has no
    // PasswordChar), so the bar talks to one of two and asks here, instead of duplicating every path.
    std::wstring BarText(TextBox^ field, PasswordBox^ secret)
    {
        return secret->Visibility == Windows::UI::Xaml::Visibility::Visible ? std::wstring(secret->Password->Data())
                                                                           : std::wstring(field->Text->Data());
    }

    // Drops every CR and LF: WP8.1's TextBox inserts "\r\n" for Enter, while the page's value is normalised to "\n".
    void StripNewlines(std::wstring& text)
    {
        text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }),
            text.end());
    }

    // The shortest replace that turns `old` into `now`: typing, deleting and pasting all arrive as one replace of
    // [start, end). Offsets follow UTF-16 units, like lw_field.
    struct FieldDiff
    {
        int start = 0;
        int end = 0;
        std::wstring inserted;
        bool empty() const { return start == end && inserted.empty(); }
    };

    FieldDiff DiffText(const std::wstring& old, const std::wstring& now)
    {
        FieldDiff diff;
        size_t prefix = 0;
        size_t shorter = std::min(old.size(), now.size());
        while (prefix < shorter && old[prefix] == now[prefix])
            ++prefix;
        size_t suffix = 0;
        while (suffix < old.size() - prefix && suffix < now.size() - prefix
            && old[old.size() - 1 - suffix] == now[now.size() - 1 - suffix])
            ++suffix;
        diff.start = static_cast<int>(prefix);
        diff.end = static_cast<int>(old.size() - suffix);
        if (diff.end < diff.start)
            diff.end = diff.start;
        diff.inserted = now.substr(prefix, now.size() - prefix - suffix);
        return diff;
    }

    // Where the caret goes after the bar's text was changed: the end of the replacement. This is derived from the
    // diff, never from TextBox::SelectionStart, which during TextChanged still holds the value from before the
    // change (measured: a 2-char commit reported caret = start+1, a backspace reported caret = start-2).
    int CaretAfter(const FieldDiff& diff, size_t length)
    {
        int caret = diff.start + static_cast<int>(diff.inserted.size());
        return caret > static_cast<int>(length) ? static_cast<int>(length) : caret;
    }

    // The control's text with every CR dropped: TextBox::Text keeps Enter as "\r\n", while a page value holds "\n".
    // Every offset the engine exchanges (and SelectionStart itself) counts a newline as one unit, so the bar works on
    // this copy and only ever writes LF back: the control widens it again by itself.
    std::wstring LineFeedText(const std::wstring& raw)
    {
        std::wstring text = raw;
        if (text.find(L'\r') != std::wstring::npos)
            text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
        return text;
    }
}

void MainPage::OnFieldEventOnUIThread(const FieldState& state, bool update)
{
    if (update)
    {
        // An UPDATE describes the field that is already being edited. If the bar is closed or shows another kind, the
        // event is late (its blur has already been handled) or belongs to a field the user is not typing in: it must
        // not reopen the bar. That is exactly what the first build did, and it produced the open/close loop with the
        // keyboard (campo: barra aperta kind 8 right after barra chiusa).
        if (!m_composeVisible || state.kind != m_barKind)
            return;
        // The bar is ahead of the engine as soon as a keystroke is in flight (the Web thread applies it later): an
        // update carrying an older sequence would rewrite the text with a snapshot missing the character the user has
        // just typed, and the next diff would then be computed against the wrong base. The counter only grows, so
        // "older than what the bar already sent" is enough to know the update is stale.
        if (state.sequence < m_textSequence)
            return;
    }
    else
    {
        if (state.kind == LW_FIELD_NONE)
        {
            HideCompose(false, L"il campo ha perso il fuoco");
            return;
        }
        // A login form that appeared after the load (a page that builds it in script): filled when a field of it
        // gets the focus, before the bar shows its text.
        if (Site::app && m_unlocked && (state.kind == LW_FIELD_PASSWORD || state.kind == LW_FIELD_TEXT || state.kind == LW_FIELD_EMAIL)
            && state.text.empty())
        {
            if (auto tab = ActiveTab())
                FillLogin(tab->view, tab->url);
        }
        ShowCompose(state);
    }

    // The engine's view of the field is authoritative: it may have clamped the text at a maxlength, or a page script
    // may have rewritten it. Rewrite the bar without echoing the change back as an edit.
    if (state.text == m_field.text && state.selectionStart == m_field.selectionStart
        && state.selectionEnd == m_field.selectionEnd)
        return;

    bool secret = state.kind == LW_FIELD_PASSWORD;
    m_field = state;
    m_syncField = true;
    if (secret)
        Secret->Password = ref new String(state.text.c_str());
    else
    {
        Field->Text = ref new String(state.text.c_str());
        int length = static_cast<int>(state.text.size());
        int start = state.selectionStart < 0 ? length : state.selectionStart;
        if (start > length)
            start = length;
        int end = state.selectionEnd < start ? start : state.selectionEnd;
        if (end > length)
            end = length;
        // SelectionStart/SelectionLength speak the page's units (a newline counts 1), even though Text stores it as
        // CRLF. Measured, not assumed: left to insert a newline itself the control used the caret index as an index
        // into the CRLF text and landed one character early for every line break already present. So the values from
        // the engine go in unchanged.
        Field->SelectionStart = start;
        Field->SelectionLength = end - start;
    }
    m_syncField = false;
}

void MainPage::ShowCompose(const FieldState& state)
{
    bool fresh = !m_composeVisible;
    if (fresh)
    {
        m_composeVisible = true;
        Compose->Visibility = Windows::UI::Xaml::Visibility::Visible;
    }
    if (!fresh && state.kind == m_barKind)
        return;

    // Kind changed (new field, or the page moved the focus): reconfigure the bar, keeping the keyboard up.
    m_barKind = state.kind;
    bool secret = state.kind == LW_FIELD_PASSWORD;
    m_syncField = true;
    Field->Text = L"";
    Secret->Password = L"";
    m_syncField = false;
    m_field = FieldState();

    Field->Visibility = secret ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    Secret->Visibility = secret ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    Secret->PasswordChar = ref new String(L"\u25CF");
    // Always true, for every kind: this is what keeps Enter from moving the focus to the address box (WP8.1 raises
    // no KeyDown for the keyboard's Enter, so there is nothing to intercept). The newline then arrives in
    // Field_TextChanged, which turns it into a line break or into lw_text_enter depending on the field kind.
    Field->AcceptsReturn = true;

    if (!secret)
    {
        // The keyboard layout follows the kind the engine classified (lw_field_kind).
        auto inputScope = ref new InputScope();
        auto scope = ref new InputScopeName();
        switch (state.kind)
        {
        case LW_FIELD_EMAIL: scope->NameValue = InputScopeNameValue::EmailSmtpAddress; break;
        case LW_FIELD_NUMBER: scope->NameValue = InputScopeNameValue::Number; break;
        case LW_FIELD_PHONE: scope->NameValue = InputScopeNameValue::TelephoneNumber; break;
        case LW_FIELD_URL: scope->NameValue = InputScopeNameValue::Url; break;
        case LW_FIELD_SEARCH: scope->NameValue = InputScopeNameValue::Search; break;
        default: scope->NameValue = InputScopeNameValue::Default; break;
        }
        inputScope->Names->Append(scope);
        Field->InputScope = inputScope;
    }

    // Focusing the bar is what raises the system keyboard; the pane's Showing event lifts the bar above it.
    if (secret)
        Secret->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    else
        Field->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    Log(L"campo: barra aperta kind " + std::to_wstring(state.kind));
}

void MainPage::HideCompose(bool notifyEngine, const wchar_t* reason)
{
    if (!m_composeVisible)
        return;
    m_composeVisible = false;
    m_barKind = LW_FIELD_NONE;
    m_syncField = true;
    Compose->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Field->Text = L"";
    Secret->Password = L"";
    m_syncField = false;
    m_field = FieldState();
    // Dropping the focus closes the keyboard too; the pane's Hiding event then sees m_composeVisible false and does
    // not call back into the engine.
    Focus(Windows::UI::Xaml::FocusState::Programmatic);
    Log(std::wstring(L"campo: barra chiusa (") + reason + L")");
    if (notifyEngine && m_view)
        engine.textBlur(m_view);
}

void MainPage::Field_TextChanged(Object^, TextChangedEventArgs^)
{
    // Programmatic updates from the engine (or the reset when the bar opens) are not edits.
    if (m_syncField || !m_composeVisible || !m_view)
        return;

    // WP8.1's TextBox stores a line break as "\r\n", while a page value holds "\n", and every offset the engine
    // exchanges counts a break as one unit. So the bar works on a LF copy: only the copy is normalised and sent, the
    // bar's own text is never rewritten for a multiline field (doing that reset the keyboard's shift state, which
    // flickered uppercase/lowercase, and moved the caret on every keystroke). Enter does not even reach this handler
    // any more: Field_KeyDown handles it and BarEnter inserts the break, because the control put it one character
    // early. What still arrives here with a line break is a paste.
    std::wstring raw = Field->Text->Data();

    if (m_barKind != LW_FIELD_MULTILINE && raw.find_first_of(L"\r\n") != std::wstring::npos)
    {
        // A line break in a single-line bar can only mean Enter: drop it and send the real Enter to the page.
        std::wstring text = raw;
        StripNewlines(text);
        m_syncField = true;
        Field->Text = ref new String(text.c_str());
        Field->SelectionStart = static_cast<int>(text.size());
        Field->SelectionLength = 0;
        m_syncField = false;
        Log(L"campo: Invio dalla tastiera");
        engine.textEnter(m_view);
        return;
    }

    SendFieldEditFor(LineFeedText(raw), m_barKind == LW_FIELD_PASSWORD);
}

void MainPage::Secret_PasswordChanged(Object^, RoutedEventArgs^)
{
    if (m_syncField || !m_composeVisible || !m_view)
        return;
    SendFieldEdit();
}

void MainPage::SendFieldEdit()
{
    bool secret = m_barKind == LW_FIELD_PASSWORD;
    SendFieldEditFor(BarText(Field, Secret), secret);
}

// Sends the difference between `now` and the text the engine last knew. Separate from SendFieldEdit() because the
// newline path has already normalised the text and must not read it back from the control.
void MainPage::SendFieldEditFor(const std::wstring& now, bool secret)
{
    FieldDiff diff = DiffText(m_field.text, now);
    if (diff.empty())
        return; // a pure caret move: the page does not need to hear about it

    int caret = secret ? static_cast<int>(now.size()) : CaretAfter(diff, now.size());
    int caretEnd = caret;
    m_textSequence += 1;

    // Optimistic, and correct because the engine applies one field's edits in order: the next diff is computed
    // against what the bar shows now, exactly like an input method's commit stream.
    m_field.text = now;
    m_field.selectionStart = caret;
    m_field.selectionEnd = caretEnd;

    engine.textEdit(m_view, m_textSequence, diff.start, diff.end, diff.inserted.c_str(), caret, caretEnd);
}

void MainPage::Field_KeyDown(Object^, KeyRoutedEventArgs^ e)
{
    // Diagnostic, and it stays: what the system keyboard really sends is not obvious on WP8.1 (measured: 13 for
    // Enter, 8 for backspace, the letters as plain codes). Control keys only: digits and letters are what the user
    // typed (codes, account numbers) and the log is plain text sent to the PC.
    if (static_cast<int>(e->Key) < 32)
        Log(L"campo: tasto " + std::to_wstring(static_cast<int>(e->Key)));
    if (e->Key != Windows::System::VirtualKey::Enter)
        return;
    // Enter belongs to the app. Left to the control it inserts the newline at its own caret index, which counts
    // newlines as one unit while Text counts them as two: measured, "ab\ncd" + Enter produced an edit at [4,4) instead
    // of [5,5), i.e. the break landed before the last character, and the error grew by one per existing line.
    e->Handled = true;
    BarEnter();
}

void MainPage::Field_LostFocus(Object^, RoutedEventArgs^)
{
    if (!m_composeVisible)
        return;
    // The bar lost the focus while it was open: whoever took it is the reason the system keyboard is about to hide.
    // Logging the type name turns "it jumps to the address bar" into a fact.
    auto focused = FocusManager::GetFocusedElement();
    std::wstring name = L"null";
    if (focused)
    {
        auto type = focused->GetType();
        if (type && type->FullName)
            name = type->FullName->Data();
    }
    Log(L"campo: barra perde il fuoco -> " + name);
}

void MainPage::Secret_KeyDown(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter || !m_view)
        return;
    e->Handled = true;
    BarEnter();
}

void MainPage::Send_Click(Object^, TappedRoutedEventArgs^)
{
    Log(L"campo: Invio toccato");
    if (!m_composeVisible)
        return;
    BarEnter();
    // Tapping a non-focusable element clears the TextBox's focus on WP8.1 (measured: FocusManager::GetFocusedElement()
    // was null 0 ms after the tap, and the system keyboard hid 63 ms later, closing the bar). A Border cannot take the
    // focus itself, so the only way to keep the keyboard up is to put the focus back right here.
    if (m_barKind == LW_FIELD_PASSWORD)
        Secret->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    else
        Field->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::BarEnter()
{
    if (!m_view || !m_composeVisible)
        return;
    if (m_barKind != LW_FIELD_MULTILINE)
    {
        // A real Enter keydown: the page's keydown handlers run and a form submits implicitly, the same path a
        // hardware keyboard takes.
        Log(L"campo: Invio inviato alla pagina");
        engine.textEnter(m_view);
        return;
    }

    // Multiline: the newline is part of the text, so it travels to the page as one ordinary lw_text_edit and the
    // page's beforeinput/input handlers and its value stay in the loop (a synthetic keydown alone does not insert
    // text: WebKit inserts from a separate char event). The insertion happens in the LF text, at the LF caret,
    // because those are the page's units; the two must not be mixed (that is the bug that put every newline one
    // character early). The control receives LF text back and widens it to CRLF by itself.
    std::wstring text = LineFeedText(Field->Text->Data());
    int start = Field->SelectionStart;
    int length = Field->SelectionLength;
    if (start < 0 || start > static_cast<int>(text.size()))
        start = static_cast<int>(text.size());
    if (length < 0 || start + length > static_cast<int>(text.size()))
        length = static_cast<int>(text.size()) - start;
    text.replace(start, length, L"\n");

    m_syncField = true;
    Field->Text = ref new String(text.c_str());
    Field->SelectionStart = start + 1;
    Field->SelectionLength = 0;
    // Read back while the handlers are still suppressed: it says whether the control interprets SelectionStart in the
    // same units the caret came from (an off-by-one per line break here would show up as a caret left of the break).
    int readBack = Field->SelectionStart;
    m_syncField = false;

    Log(L"campo: a capo in " + std::to_wstring(start) + L", caret " + std::to_wstring(readBack)
        + L", LF " + std::to_wstring(text.size()) + L", CRLF " + std::to_wstring(Field->Text->Length()));
    SendFieldEditFor(text, false);
}

void MainPage::InputPane_Showing(Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e)
{
    // Editing the address: the bar rises above the keyboard by itself, and the suggestions fill exactly the space left
    // above it, scrolling when they do not fit. Left to the system, the whole page was pushed up instead and the top of
    // the list went off the screen, out of reach.
    if (Address->FocusState != Windows::UI::Xaml::FocusState::Unfocused)
    {
        e->EnsuredFocusedElementInView = true;
        BarShift->Y = -e->OccludedRect.Height;
        Suggest->Margin = Thickness(0, 0, 0, e->OccludedRect.Height);
    }
    // Lift the bar by the height the keyboard occupies. The page view is deliberately not resized: a resize means a
    // full relayout of the page at the moment the keyboard appears, one of the most expensive things WebCore does.
    Compose->Margin = Thickness(0, 0, 0, e->OccludedRect.Height);
    FindBar->Margin = Thickness(0, 0, 0, e->OccludedRect.Height);
    Log(L"tastiera: mostrata, occlusa " + std::to_wstring(static_cast<int>(e->OccludedRect.Height)) + L" px");
    if (m_selectAddressOnKeyboard)
    {
        m_selectAddressOnKeyboard = false;
        Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([this]() { Address->SelectAll(); }));
    }
}

void MainPage::InputPane_Hiding(Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^)
{
    BarShift->Y = 0;
    Suggest->Margin = Thickness(0);
    Compose->Margin = Thickness(0);
    FindBar->Margin = Thickness(0);
    Log(L"tastiera: nascosta");
    // The user dismissed the keyboard: that closes the bar and tells the page to drop the focus.
    if (m_composeVisible)
        HideCompose(true, L"tastiera nascosta");
}

// ---- long press: menu and text panel ----

void MainPage::OnContextMenuOnUIThread(const std::wstring& link, const std::wstring& image, const std::wstring& text)
{
    if (link.empty() && image.empty())
    {
        // Plain text: straight to the panel, where the system selection picks words.
        if (!text.empty())
            ShowSheet(text, false);
        return;
    }
    MenuItems->Children->Clear();
    MenuTitle->Text = ref new String((link.empty() ? image : link).c_str());
    if (!link.empty())
    {
        AddMenuItem(L"apri", [this, link]() { Navigate(link); });
        AddMenuItem(L"copia link", [this, link]() { ShowSheet(link, true); });
        AddMenuItem(L"condividi link", [this, link]() { Share(link, true); });
        if (!text.empty())
            AddMenuItem(L"copia testo", [this, text]() { ShowSheet(text, false); });
        if (engine.download)
            AddMenuItem(L"scarica link", [this, link]() { if (m_view) engine.download(m_view, link.c_str()); });
    }
    if (!image.empty())
    {
        AddMenuItem(L"apri immagine", [this, image]() { Navigate(image); });
        // Into the phone's pictures (Immagini salvate), through a download with the page's cookies.
        if (engine.download)
        {
            AddMenuItem(L"salva immagine", [this, image]()
            {
                if (!m_view)
                    return;
                m_imageSaves.push_back(image);
                engine.download(m_view, image.c_str());
            });
        }
        AddMenuItem(L"copia indirizzo immagine", [this, image]() { ShowSheet(image, true); });
    }
    Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::AddMenuItem(const wchar_t* label, std::function<void()> action)
{
    auto item = ref new Border();
    item->Background = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x1F, 0x1F, 0x1F));
    item->Padding = Thickness(16, 14, 16, 14);
    auto caption = ref new TextBlock();
    caption->Text = ref new String(label);
    caption->FontSize = 24;
    caption->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
    item->Child = caption;
    item->Tapped += ref new TappedEventHandler([this, action](Object^, TappedRoutedEventArgs^ e)
    {
        e->Handled = true;
        Menu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        action();
    });
    MenuItems->Children->Append(item);
}

void MainPage::Menu_Tapped(Object^, TappedRoutedEventArgs^)
{
    Menu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::ShowSheet(const std::wstring& text, bool link)
{
    m_sheetText = text;
    m_sheetIsLink = link;
    SheetText->Text = ref new String(text.c_str());
    Sheet->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::SheetShare_Tapped(Object^, TappedRoutedEventArgs^)
{
    // The selection if there is one, else the whole text.
    std::wstring selected = SheetText->SelectedText ? SheetText->SelectedText->Data() : L"";
    Share(selected.empty() ? m_sheetText : selected, m_sheetIsLink && selected.empty());
}

void MainPage::SheetClose_Tapped(Object^, TappedRoutedEventArgs^)
{
    Sheet->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::Share(const std::wstring& text, bool link)
{
    m_shareText = text;
    m_shareIsLink = link;
    Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI();
}

void MainPage::ShareRequested(Windows::ApplicationModel::DataTransfer::DataTransferManager^,
                              Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^ e)
{
    auto data = e->Request->Data;
    data->Properties->Title = L"Internet";
    if (!m_shareFile.empty())
    {
        // A downloaded file: the share target gets the file itself.
        std::wstring path = m_shareFile;
        m_shareFile.clear();
        auto deferral = e->Request->GetDeferral();
        create_task(StorageFile::GetFileFromPathAsync(ref new String(path.c_str()))).then([data, deferral](task<StorageFile^> t)
        {
            try
            {
                auto items = ref new Platform::Collections::Vector<IStorageItem^>();
                items->Append(t.get());
                data->SetStorageItems(items);
            }
            catch (Exception^)
            {
            }
            deferral->Complete();
        });
        return;
    }
    if (m_shareIsLink)
    {
        try
        {
            data->SetWebLink(ref new Windows::Foundation::Uri(ref new String(m_shareText.c_str())));
            return;
        }
        catch (Exception^)
        {
        }
    }
    data->SetText(ref new String(m_shareText.c_str()));
}

// ---- tabs ----
// Each live tab is an engine view drawing into its own SwapChainPanel in Pages; only the active panel is visible and
// only the active view renders (lw_view_set_visible: the others are hidden to their pages, timers throttled). Touch
// goes to the active view through inputView. A tab beyond maxLiveTabs, or any hidden tab when memory runs short, is
// discarded: its view and panel go, its address stays, and it loads again when shown.

std::shared_ptr<Tab> MainPage::ActiveTab()
{
    return m_activeTab;
}

std::shared_ptr<Tab> MainPage::TabForView(lw_view* view)
{
    if (!view)
        return nullptr;
    for (auto& tab : m_tabs)
    {
        if (tab->view == view)
            return tab;
    }
    return nullptr;
}

std::shared_ptr<Tab> MainPage::NewTab(const std::wstring& url, int openerId, bool activate)
{
    auto tab = std::make_shared<Tab>();
    tab->id = m_nextTabId++;
    tab->url = url;
    tab->title = url;
    tab->openerId = openerId;
    // Next to the tab that opened it, else at the end.
    auto position = m_tabs.end();
    for (auto it = m_tabs.begin(); openerId && it != m_tabs.end(); ++it)
    {
        if ((*it)->id == openerId)
        {
            position = it + 1;
            break;
        }
    }
    m_tabs.insert(position, tab);
    Log(L"scheda " + std::to_wstring(tab->id) + L" nuova: " + url);
    if (activate)
        ActivateTab(tab);
    UpdateBar();
    return tab;
}

void MainPage::EnsureLive(const std::shared_ptr<Tab>& tab)
{
    if (tab->view || m_viewWidth <= 0)
        return;
    auto panel = ref new SwapChainPanel();
    tab->panel = panel;
    Pages->Children->Append(panel);
    StartInput(panel);
    tab->view = engine.viewCreate(reinterpret_cast<void*>(panel), m_viewWidth, m_viewHeight, m_viewScale);
    Log(L"scheda " + std::to_wstring(tab->id) + L": vista " + std::to_wstring(m_viewWidth) + L"x" + std::to_wstring(m_viewHeight));
    engine.loadURL(tab->view, tab->url.empty() ? L"about:blank" : tab->url.c_str());
}

void MainPage::ActivateTab(const std::shared_ptr<Tab>& tab)
{
    if (!tab)
        return;
    auto previous = m_activeTab;
    // Fullscreen belongs to the tab that asked for it: another tab in front ends it.
    if (m_fullscreenView && tab->view != m_fullscreenView)
    {
        if (previous && previous->view == m_fullscreenView && engine.exitFullscreen)
            engine.exitFullscreen(m_fullscreenView);
        ShowFullscreen(nullptr);
    }
    if (previous && previous != tab)
    {
        if (m_composeVisible)
            HideCompose(true, L"cambio scheda");
        HideFind();
        if (previous->view)
            engine.setVisible(previous->view, 0);
        if (previous->panel)
            previous->panel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    }
    m_activeTab = tab;
    tab->lastUsed = GetTickCount64();
    EnsureLive(tab);
    if (tab->panel)
        tab->panel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    if (tab->view)
        engine.setVisible(tab->view, 1);
    m_view = tab->view;
    inputView.store(tab->view);
    if (tab->input)
        touchDispatcher = tab->input->Dispatcher;
    m_currentURL = tab->url;
    Address->Text = ref new String(tab->url == L"about:blank" ? L"" : tab->url.c_str());
    DiscardBeyond(maxLiveTabs);
    UpdateBar();
}

void MainPage::CloseTab(std::shared_ptr<Tab> tab)
{
    auto it = std::find(m_tabs.begin(), m_tabs.end(), tab);
    if (it == m_tabs.end())
        return;
    size_t index = static_cast<size_t>(it - m_tabs.begin());
    Discard(tab, L"chiusa");
    m_tabs.erase(it);
    Log(L"scheda " + std::to_wstring(tab->id) + L" chiusa");
    if (tab != m_activeTab)
    {
        UpdateBar();
        return;
    }
    m_activeTab = nullptr;
    if (m_tabs.empty())
    {
        // The last one closed: an empty tab, with the favorites to pick from.
        NewTab(L"about:blank", 0, true);
        ShowList(ListKind::Favorites);
        return;
    }
    ActivateTab(m_tabs[std::min(index, m_tabs.size() - 1)]);
}

void MainPage::Discard(const std::shared_ptr<Tab>& tab, const wchar_t* reason)
{
    if (!tab->view)
        return;
    if (tab->view == m_view)
    {
        if (m_composeVisible)
            HideCompose(false, L"scheda scartata");
        m_view = nullptr;
        inputView.store(nullptr);
    }
    // The engine answers the page's open dialog itself (lw_view_destroy); the panel goes with it.
    if (m_dialogView == tab->view)
    {
        m_dialogView = nullptr;
        DialogPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    }
    engine.viewDestroy(tab->view);
    tab->view = nullptr;
    if (tab->input)
    {
        // The panel's input thread ends its loop (it is a thread-pool thread, not to be kept forever).
        auto inputDispatcher = tab->input->Dispatcher;
        inputDispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([inputDispatcher]()
        {
            inputDispatcher->StopProcessEvents();
        }));
        tab->input = nullptr;
    }
    unsigned int index = 0;
    if (tab->panel && Pages->Children->IndexOf(tab->panel, &index))
        Pages->Children->RemoveAt(index);
    tab->panel = nullptr;
    tab->loading = false;
    tab->canGoBack = false;
    tab->canGoForward = false;
    Log(L"scheda " + std::to_wstring(tab->id) + L" scartata (" + reason + L"): " + tab->url);
}

// Keeps at most `live` tabs with a page, the active one included; the least recently shown go first.
void MainPage::DiscardBeyond(size_t live)
{
    std::vector<std::shared_ptr<Tab>> hidden;
    for (auto& tab : m_tabs)
    {
        // The shown tab's opener stays too: a sign-in popup posts its result back to it.
        if (tab->view && tab != m_activeTab && !(m_activeTab && tab->id == m_activeTab->openerId))
            hidden.push_back(tab);
    }
    size_t keep = live > 1 ? live - 2 : 0;
    if (!m_activeTab || !m_activeTab->openerId)
        keep = live > 0 ? live - 1 : 0;
    if (hidden.size() <= keep)
        return;
    std::sort(hidden.begin(), hidden.end(), [](const std::shared_ptr<Tab>& a, const std::shared_ptr<Tab>& b)
    {
        return a->lastUsed < b->lastUsed;
    });
    for (size_t i = 0; i + keep < hidden.size(); ++i)
        Discard(hidden[i], live == 1 ? L"memoria quasi finita" : L"troppe schede aperte");
}

// The bar follows the active tab: tab count, reload or stop, the load line.
void MainPage::UpdateBar()
{
    TabsCount->Text = ref new String(std::to_wstring(m_tabs.size()).c_str());
    auto tab = ActiveTab();
    bool loading = tab && tab->loading;
    ReloadGlyph->Text = loading ? L"\u2715" : L"\u21BB";
    if (!tab)
        return;
    if (tab->loading)
    {
        SetLoadLine(true, false);
        LoadScale->ScaleX = static_cast<double>(std::max(tab->progress, 5)) / 100;
    }
    else
        SetLoadLine(tab->failed, tab->failed);
}

// window.open: a tab next to its opener, whose page the engine creates at once (lw_window_reply) while the opener's
// script waits; WebCore then loads the address into it. If the engine has stopped waiting, the tab loads the address
// as an ordinary one, without an opener.
void MainPage::OpenWindow(lw_view* openerView, int id, const std::wstring& url)
{
    auto opener = TabForView(openerView);
    if (!engine.windowReply || !opener || m_viewWidth <= 0)
    {
        if (engine.windowReply)
            engine.windowReply(openerView, id, nullptr, 0, 0, 0);
        return;
    }
    auto tab = std::make_shared<Tab>();
    tab->id = m_nextTabId++;
    tab->url = url.empty() ? L"about:blank" : url;
    tab->title = tab->url;
    tab->openerId = opener->id;
    auto position = std::find(m_tabs.begin(), m_tabs.end(), opener);
    m_tabs.insert(position == m_tabs.end() ? position : position + 1, tab);
    auto panel = ref new SwapChainPanel();
    tab->panel = panel;
    Pages->Children->Append(panel);
    StartInput(panel);
    tab->view = engine.windowReply(openerView, id, reinterpret_cast<void*>(panel), m_viewWidth, m_viewHeight, m_viewScale);
    if (tab->view)
        Log(L"scheda " + std::to_wstring(tab->id) + L": finestra della pagina (window.open) " + url);
    else
    {
        Log(L"scheda " + std::to_wstring(tab->id) + L": window.open arrivata tardi, apro l'indirizzo " + url);
        tab->view = engine.viewCreate(reinterpret_cast<void*>(panel), m_viewWidth, m_viewHeight, m_viewScale);
        engine.loadURL(tab->view, tab->url.c_str());
    }
    ActivateTab(tab);
}

void MainPage::OpenFromLaunch(const std::wstring& url)
{
    if (url.empty())
        return;
    if (m_viewWidth <= 0)
    {
        m_pendingLaunchURL = url;
        return;
    }
    for (auto& tab : m_tabs)
    {
        if (tab->url == url)
        {
            ActivateTab(tab);
            return;
        }
    }
    NewTab(url, 0, true);
}

// ---- favorites and history ----
// Favorites: favorites.txt, "title<TAB>url" per line, in the user's order. History: history.txt, "time<TAB>title<TAB>url",
// oldest first, the last 500 visits; written at suspension. Both UTF-8 in LocalFolder.

namespace
{
    const size_t maxHistory = 500;

    std::wstring LocalPath(const wchar_t* name)
    {
        return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + name;
    }

    bool ReadTextFile(const std::wstring& path, std::wstring& text)
    {
        HANDLE file = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        std::string bytes;
        char buffer[16384];
        DWORD read = 0;
        while (ReadFile(file, buffer, sizeof buffer, &read, nullptr) && read)
            bytes.append(buffer, read);
        CloseHandle(file);
        int n = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), nullptr, 0);
        text.assign(n > 0 ? n : 0, L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), &text[0], n);
        return true;
    }

    void WriteTextFile(const std::wstring& path, const std::wstring& text)
    {
        HANDLE file = CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        std::string bytes = Utf8(text);
        DWORD written = 0;
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
    }

    std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator)
    {
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (;;)
        {
            size_t end = text.find(separator, start);
            parts.push_back(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (end == std::wstring::npos)
                break;
            start = end + 1;
        }
        return parts;
    }

    // Titles and addresses go on one line of a file: no tabs or line breaks inside.
    std::wstring OneLine(const std::wstring& text)
    {
        std::wstring line = text;
        for (auto& c : line)
        {
            if (c == L'\t' || c == L'\r' || c == L'\n')
                c = L' ';
        }
        return line;
    }

    std::wstring Lower(const std::wstring& text)
    {
        std::wstring lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        return lower;
    }

    // example.com from https://www.example.com/path.
    std::wstring HostOf(const std::wstring& url)
    {
        size_t scheme = url.find(L"://");
        if (scheme == std::wstring::npos)
            return std::wstring();
        size_t start = scheme + 3;
        size_t end = url.find_first_of(L"/?#:", start);
        std::wstring host = Lower(url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (host.compare(0, 4, L"www.") == 0)
            host = host.substr(4);
        return host;
    }

    long long Now()
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        return (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    }

    // "oggi 14:05", "ieri 09:12", "27/10 18:40": when a history entry was visited, in local time (a WinRT calendar:
    // FileTimeToLocalFileTime is not in the phone's app API).
    std::wstring WhenText(long long time)
    {
        Windows::Foundation::DateTime then;
        then.UniversalTime = time;
        auto calendar = ref new Windows::Globalization::Calendar();
        calendar->ChangeClock(L"24HourClock");
        calendar->SetToNow();
        int todayYear = calendar->Year, todayMonth = calendar->Month, todayDay = calendar->Day;
        calendar->AddDays(-1);
        int yesterdayYear = calendar->Year, yesterdayMonth = calendar->Month, yesterdayDay = calendar->Day;
        calendar->SetDateTime(then);
        wchar_t clock[8];
        swprintf_s(clock, L"%02d:%02d", calendar->Hour, calendar->Minute);
        if (calendar->Year == todayYear && calendar->Month == todayMonth && calendar->Day == todayDay)
            return std::wstring(L"oggi ") + clock;
        if (calendar->Year == yesterdayYear && calendar->Month == yesterdayMonth && calendar->Day == yesterdayDay)
            return std::wstring(L"ieri ") + clock;
        wchar_t date[16];
        swprintf_s(date, L"%02d/%02d ", calendar->Day, calendar->Month);
        return date + std::wstring(clock);
    }

    // The site a "versione desktop" choice belongs to: youtube.com for m.youtube.com and www.youtube.com, bbc.co.uk for
    // www.bbc.co.uk. Not the public suffix list, but right for the sites a phone visits.
    std::wstring SiteOf(const std::wstring& url)
    {
        std::wstring host = HostOf(url);
        std::vector<std::wstring> labels = Split(host, L'.');
        if (labels.size() <= 2)
            return host;
        const std::wstring& second = labels[labels.size() - 2];
        bool shortTopLevel = labels.back().size() == 2;
        bool commonSecondLevel = second == L"co" || second == L"com" || second == L"org" || second == L"net"
            || second == L"gov" || second == L"edu" || second == L"ac";
        size_t keep = shortTopLevel && commonSecondLevel ? 3 : 2;
        std::wstring site;
        for (size_t i = labels.size() - keep; i < labels.size(); ++i)
            site += (site.empty() ? L"" : L".") + labels[i];
        return site;
    }
}

void MainPage::LoadEntries()
{
    std::wstring text;
    if (ReadTextFile(LocalPath(L"favorites.txt"), text))
    {
        for (auto& line : Split(text, L'\n'))
        {
            auto fields = Split(line, L'\t');
            if (fields.size() < 2 || fields[1].empty())
                continue;
            PageEntry entry;
            entry.title = fields[0];
            entry.url = fields[1];
            m_favorites.push_back(entry);
        }
    }
    else
    {
        // First start: the test pages (the old "Prove" button) and the sites tried so far.
        const wchar_t* const defaults[][2] = {
            { L"Prove browser", startPage },
            { L"Google", L"https://www.google.com/" },
            { L"YouTube", L"https://m.youtube.com/" },
            { L"Wikipedia", L"https://it.m.wikipedia.org/" },
            { L"Claude", L"https://claude.ai/" },
        };
        for (auto& item : defaults)
        {
            PageEntry entry;
            entry.title = item[0];
            entry.url = item[1];
            m_favorites.push_back(entry);
        }
        SaveFavorites();
    }
    text.clear();
    if (ReadTextFile(LocalPath(L"history.txt"), text))
    {
        for (auto& line : Split(text, L'\n'))
        {
            auto fields = Split(line, L'\t');
            if (fields.size() < 3 || fields[2].empty())
                continue;
            PageEntry entry;
            entry.time = _wtoi64(fields[0].c_str());
            entry.title = fields[1];
            entry.url = fields[2];
            m_historyEntries.push_back(entry);
        }
        if (m_historyEntries.size() > maxHistory)
            m_historyEntries.erase(m_historyEntries.begin(), m_historyEntries.end() - maxHistory);
    }
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (values->HasKey(L"siteIdentities"))
    {
        for (auto& line : Split(safe_cast<String^>(values->Lookup(L"siteIdentities"))->Data(), L'\n'))
        {
            auto fields = Split(line, L'\t');
            if (fields.size() == 2 && !fields[0].empty())
                m_siteIdentities.push_back(std::make_pair(fields[0], fields[1]));
        }
    }
    if (values->HasKey(L"desktopSites"))
    {
        for (auto& site : Split(safe_cast<String^>(values->Lookup(L"desktopSites"))->Data(), L'\n'))
        {
            if (!site.empty())
                m_desktopSites.push_back(site);
        }
    }
}

void MainPage::SaveFavorites()
{
    std::wstring text;
    for (auto& entry : m_favorites)
        text += OneLine(entry.title) + L"\t" + OneLine(entry.url) + L"\n";
    WriteTextFile(LocalPath(L"favorites.txt"), text);
}

void MainPage::SaveHistory()
{
    if (!m_historyDirty)
        return;
    m_historyDirty = false;
    std::wstring text;
    for (auto& entry : m_historyEntries)
        text += std::to_wstring(entry.time) + L"\t" + OneLine(entry.title) + L"\t" + OneLine(entry.url) + L"\n";
    WriteTextFile(LocalPath(L"history.txt"), text);
}

// A committed page (title still empty) or its title arriving later: one entry per visit, updated in place. Tabs
// load in parallel, so the entry for this address is looked for among the last few rather than only the last one.
void MainPage::AddHistory(const std::wstring& url, const std::wstring& title)
{
    // A site app keeps no history (nothing would show it, and it is a bank's pages).
    if (Site::app)
        return;
    if (url.compare(0, 7, L"http://") != 0 && url.compare(0, 8, L"https://") != 0)
        return;
    size_t checked = 0;
    for (auto it = m_historyEntries.rbegin(); it != m_historyEntries.rend() && checked < 8; ++it, ++checked)
    {
        if (it->url != url)
            continue;
        // The same page again a moment later (its title, a reload): the entry is refreshed, not repeated.
        if (!title.empty())
            it->title = title;
        else if (Now() - it->time > 30 * 10000000LL)
            break;
        it->time = Now();
        m_historyDirty = true;
        return;
    }
    PageEntry entry;
    entry.url = url;
    entry.title = title.empty() ? url : title;
    entry.time = Now();
    m_historyEntries.push_back(entry);
    if (m_historyEntries.size() > maxHistory)
        m_historyEntries.erase(m_historyEntries.begin());
    m_historyDirty = true;
}

bool MainPage::IsFavorite(const std::wstring& url)
{
    for (auto& entry : m_favorites)
    {
        if (entry.url == url)
            return true;
    }
    return false;
}

void MainPage::ToggleFavorite(const std::wstring& url, const std::wstring& title)
{
    for (auto it = m_favorites.begin(); it != m_favorites.end(); ++it)
    {
        if (it->url == url)
        {
            m_favorites.erase(it);
            SaveFavorites();
            return;
        }
    }
    PageEntry entry;
    entry.url = url;
    entry.title = title.empty() || title == url ? HostOf(url) : title;
    m_favorites.push_back(entry);
    SaveFavorites();
}

// One row of the list page: title and address, a tap opens, a long press offers more, an X at the end closes (tabs).
void MainPage::AddListRow(const std::wstring& title, const std::wstring& detail, std::function<void()> open,
                          std::function<void()> hold, std::function<void()> close)
{
    auto row = ref new Grid();
    row->Background = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x1F, 0x1F, 0x1F));
    auto column = ref new ColumnDefinition();
    column->Width = GridLength(1, GridUnitType::Star);
    row->ColumnDefinitions->Append(column);
    auto column2 = ref new ColumnDefinition();
    column2->Width = GridLength(1, GridUnitType::Auto);
    row->ColumnDefinitions->Append(column2);

    auto text = ref new StackPanel();
    text->Margin = Thickness(16, 10, 8, 10);
    auto caption = ref new TextBlock();
    caption->Text = ref new String(title.c_str());
    caption->FontSize = 21;
    caption->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
    caption->TextTrimming = TextTrimming::CharacterEllipsis;
    text->Children->Append(caption);
    auto detailBlock = ref new TextBlock();
    detailBlock->Text = ref new String(detail.c_str());
    detailBlock->FontSize = 14;
    detailBlock->Foreground = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0xAA, 0xAA, 0xAA));
    detailBlock->TextTrimming = TextTrimming::CharacterEllipsis;
    text->Children->Append(detailBlock);
    row->Children->Append(text);

    row->Tapped += ref new TappedEventHandler([open](Object^, TappedRoutedEventArgs^ e)
    {
        e->Handled = true;
        if (open)
            open();
    });
    if (hold)
    {
        row->Holding += ref new HoldingEventHandler([hold](Object^, HoldingRoutedEventArgs^ e)
        {
            e->Handled = true;
            if (e->HoldingState == Windows::UI::Input::HoldingState::Started)
                hold();
        });
    }
    if (close)
    {
        auto x = ref new Border();
        x->Background = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x44, 0x44, 0x44));
        x->Width = 56;
        x->Margin = Thickness(0, 6, 8, 6);
        auto glyph = ref new TextBlock();
        glyph->Text = L"X";
        glyph->FontSize = 20;
        glyph->FontWeight = Windows::UI::Text::FontWeights::SemiBold;
        glyph->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
        glyph->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        glyph->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        x->Child = glyph;
        Grid::SetColumn(x, 1);
        x->Tapped += ref new TappedEventHandler([close](Object^, TappedRoutedEventArgs^ e)
        {
            e->Handled = true;
            close();
        });
        row->Children->Append(x);
    }
    ListItems->Children->Append(row);
}

void MainPage::ShowList(ListKind kind)
{
    if (m_listKind == ListKind::Files && kind != ListKind::Files && m_pickId)
        AnswerPick(std::vector<std::wstring>());
    if (m_listKind == ListKind::Picker && m_pickerId)
        AnswerPicker(nullptr);
    m_listKind = kind;
    ListItems->Children->Clear();
    auto white = ref new SolidColorBrush(Windows::UI::Colors::White);
    auto gray = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x77, 0x77, 0x77));
    std::wstring empty;
    if (kind == ListKind::Favorites)
    {
        ListTitle1->Text = L"preferiti";
        ListTitle1->Foreground = white;
        ListTitle2->Text = L"cronologia";
        ListTitle2->Foreground = gray;
        for (size_t i = 0; i < m_favorites.size(); ++i)
        {
            PageEntry entry = m_favorites[i];
            AddListRow(entry.title, entry.url, [this, entry]()
            {
                HideList();
                Navigate(entry.url);
            }, [this, entry]()
            {
                MenuItems->Children->Clear();
                MenuTitle->Text = ref new String(entry.title.c_str());
                AddMenuItem(L"apri in una nuova scheda", [this, entry]() { HideList(); NewTab(entry.url, 0, true); });
                AddMenuItem(L"rinomina", [this, entry]()
                {
                    ShowAppDialog(L"rinomina preferito", entry.url, entry.title, L"salva", [this, entry](bool ok, const std::wstring& value)
                    {
                        if (!ok || value.empty())
                            return;
                        for (auto& favorite : m_favorites)
                        {
                            if (favorite.url == entry.url)
                                favorite.title = value;
                        }
                        SaveFavorites();
                        ShowList(ListKind::Favorites);
                    });
                });
                AddMenuItem(L"sposta in alto", [this, entry]()
                {
                    for (size_t j = 1; j < m_favorites.size(); ++j)
                    {
                        if (m_favorites[j].url == entry.url)
                        {
                            std::swap(m_favorites[j], m_favorites[j - 1]);
                            break;
                        }
                    }
                    SaveFavorites();
                    ShowList(ListKind::Favorites);
                });
                AddMenuItem(L"elimina", [this, entry]()
                {
                    ToggleFavorite(entry.url, entry.title);
                    ShowList(ListKind::Favorites);
                });
                Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
            }, nullptr);
        }
        bool web = m_currentURL.compare(0, 4, L"http") == 0;
        ListActionText->Text = !web ? L"cronologia" : IsFavorite(m_currentURL) ? L"togli questa pagina" : L"aggiungi questa pagina";
        empty = L"Nessun preferito. Apri una pagina e tocca \u201Caggiungi questa pagina\u201D.";
    }
    else if (kind == ListKind::History)
    {
        ListTitle1->Text = L"preferiti";
        ListTitle1->Foreground = gray;
        ListTitle2->Text = L"cronologia";
        ListTitle2->Foreground = white;
        // Newest first; the most recent 150 (the list is built as XAML rows, so not all 500).
        size_t shown = 0;
        for (auto it = m_historyEntries.rbegin(); it != m_historyEntries.rend() && shown < 150; ++it, ++shown)
        {
            PageEntry entry = *it;
            AddListRow(entry.title, WhenText(entry.time) + L"  " + entry.url, [this, entry]()
            {
                HideList();
                Navigate(entry.url);
            }, [this, entry]()
            {
                MenuItems->Children->Clear();
                MenuTitle->Text = ref new String(entry.url.c_str());
                AddMenuItem(L"apri in una nuova scheda", [this, entry]() { HideList(); NewTab(entry.url, 0, true); });
                AddMenuItem(IsFavorite(entry.url) ? L"togli dai preferiti" : L"aggiungi ai preferiti", [this, entry]()
                {
                    ToggleFavorite(entry.url, entry.title);
                });
                AddMenuItem(L"elimina dalla cronologia", [this, entry]()
                {
                    m_historyEntries.erase(std::remove_if(m_historyEntries.begin(), m_historyEntries.end(), [&entry](const PageEntry& other)
                    {
                        return other.url == entry.url;
                    }), m_historyEntries.end());
                    m_historyDirty = true;
                    SaveHistory();
                    ShowList(ListKind::History);
                });
                Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
            }, nullptr);
        }
        ListActionText->Text = L"cancella cronologia";
        empty = L"La cronologia \u00E8 vuota.";
    }
    else if (kind == ListKind::Tabs)
    {
        ListTitle1->Text = L"schede";
        ListTitle1->Foreground = white;
        ListTitle2->Text = L"";
        for (auto& tab : m_tabs)
        {
            auto entry = tab;
            std::wstring title = tab->title.empty() || tab->title == L"about:blank" ? L"nuova scheda" : tab->title;
            std::wstring detail = (tab == m_activeTab ? L"\u25B6 " : tab->view ? L"" : L"(da ricaricare) ")
                + (tab->url == L"about:blank" ? L"" : tab->url);
            AddListRow(title, detail, [this, entry]()
            {
                HideList();
                ActivateTab(entry);
            }, nullptr, [this, entry]()
            {
                CloseTab(entry);
                if (ListPanel->Visibility == Windows::UI::Xaml::Visibility::Visible && m_listKind == ListKind::Tabs)
                    ShowList(ListKind::Tabs);
            });
        }
        ListActionText->Text = L"nuova scheda";
    }
    else if (kind == ListKind::Downloads)
    {
        LoadDownloads();
        ListTitle1->Text = L"download";
        ListTitle1->Foreground = white;
        ListTitle2->Text = L"";
        for (auto it = m_downloads.rbegin(); it != m_downloads.rend(); ++it)
        {
            DownloadEntry entry = *it;
            auto megabytes = [](long long bytes)
            {
                wchar_t text[32];
                if (bytes < 1024 * 1024)
                    swprintf_s(text, L"%lld KB", (bytes + 1023) / 1024);
                else
                    swprintf_s(text, L"%.1f MB", bytes / 1048576.0);
                return std::wstring(text);
            };
            std::wstring detail;
            if (entry.state == 0)
                detail = megabytes(entry.received) + (entry.total > 0 ? L" di " + megabytes(entry.total) : L"") + L", in corso";
            else if (entry.state == 1)
                detail = L"completato, " + megabytes(entry.received);
            else if (entry.state == 2)
                detail = L"non riuscito: " + entry.error;
            else
                detail = L"annullato";
            AddListRow(entry.name, detail, [this, entry]()
            {
                if (entry.state == 1)
                    OpenFile(entry.path);
                else
                    ShowDownloadMenu(entry);
            }, [this, entry]() { ShowDownloadMenu(entry); }, nullptr);
        }
        ListActionText->Text = L"pulisci elenco";
        empty = L"Nessun download. I file che le pagine non mostrano (PDF, archivi, musica...) vanno nella cartella Download\\Internet del telefono (app File) e sono elencati qui.";
    }
    ListEmpty->Text = ref new String(empty.c_str());
    ListEmpty->Visibility = ListItems->Children->Size == 0 && !empty.empty() ? Windows::UI::Xaml::Visibility::Visible
                                                                              : Windows::UI::Xaml::Visibility::Collapsed;
    ListScroll->ChangeView(nullptr, ref new Box<double>(0.0), nullptr, true);
    ListPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::HideList()
{
    // The page's file list closed without a file: its <input> hears "cancel".
    if (m_listKind == ListKind::Files && m_pickId)
        AnswerPick(std::vector<std::wstring>());
    // A <select> or colour list closed without a choice.
    if (m_listKind == ListKind::Picker && m_pickerId)
        AnswerPicker(nullptr);
    m_listKind = ListKind::None;
    ListPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    ListItems->Children->Clear();
}

void MainPage::ListTitle1_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    if (m_listKind == ListKind::History)
        ShowList(ListKind::Favorites);
}

void MainPage::ListTitle2_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    if (m_listKind == ListKind::Favorites)
        ShowList(ListKind::History);
    else if (m_listKind == ListKind::Files && m_pickMultiple)
        PickPhotos();
}

void MainPage::ListAction_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    if (m_listKind == ListKind::Favorites)
    {
        if (m_currentURL.compare(0, 4, L"http") != 0)
        {
            ShowList(ListKind::History);
            return;
        }
        auto tab = ActiveTab();
        ToggleFavorite(m_currentURL, tab ? tab->title : std::wstring());
        ShowList(ListKind::Favorites);
    }
    else if (m_listKind == ListKind::History)
    {
        ShowAppDialog(L"cronologia", L"Cancellare tutta la cronologia?", std::wstring(), L"cancella", [this](bool ok, const std::wstring&)
        {
            if (!ok)
                return;
            m_historyEntries.clear();
            m_historyDirty = true;
            SaveHistory();
            // Links stop showing as visited too.
            if (engine.clearVisitedLinks)
                engine.clearVisitedLinks();
            ShowList(ListKind::History);
        });
    }
    else if (m_listKind == ListKind::Picker)
        HideList();
    else if (m_listKind == ListKind::Tabs)
    {
        HideList();
        NewTab(L"about:blank", 0, true);
        Address->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    }
    else if (m_listKind == ListKind::Files)
    {
        // One file: "foto" opens the photo picker instead. Several: "allega" sends the marked ones.
        if (!m_pickMultiple)
        {
            PickPhotos();
            return;
        }
        if (m_pickSelected.empty())
            return;
        std::vector<std::wstring> paths = m_pickSelected;
        AnswerPick(paths);
        HideList();
    }
    else if (m_listKind == ListKind::Downloads)
    {
        // Only the entries: files that arrived stay (elimina, in the long-press menu, removes one).
        m_downloads.erase(std::remove_if(m_downloads.begin(), m_downloads.end(), [](const DownloadEntry& entry)
        {
            return entry.state >= 2;
        }), m_downloads.end());
        ShowList(ListKind::Downloads);
    }
}

void MainPage::ListClose_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    HideList();
}

// ---- the page's form pickers (LW_EVENT_PICKER) ----

namespace
{
    std::vector<std::wstring> Lines(const std::wstring& text)
    {
        std::vector<std::wstring> lines;
        for (size_t start = 0; start < text.size();)
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos)
                end = text.size();
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return lines;
    }

    // "#rrggbb" as 0xRRGGBB, or -1.
    int ParseColor(const std::wstring& text)
    {
        if (text.size() != 7 || text[0] != L'#')
            return -1;
        wchar_t* end = nullptr;
        long value = wcstol(text.c_str() + 1, &end, 16);
        return end == text.c_str() + 7 ? static_cast<int>(value) : -1;
    }

    Windows::Globalization::Calendar^ GregorianNow()
    {
        auto calendar = ref new Windows::Globalization::Calendar();
        calendar->ChangeCalendarSystem(Windows::Globalization::CalendarIdentifiers::Gregorian);
        calendar->SetToNow();
        return calendar;
    }

    std::wstring TwoDigits(int value)
    {
        wchar_t text[16];
        swprintf_s(text, L"%02d", value);
        return text;
    }

    std::wstring FourDigits(int value)
    {
        wchar_t text[16];
        swprintf_s(text, L"%04d", value);
        return text;
    }

    // Days from 1970-01-01 to a Gregorian date (Howard Hinnant's days_from_civil).
    long long DaysFromCivil(int y, int m, int d)
    {
        y -= m <= 2;
        long long era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<long long>(doe) - 719468;
    }

    // The ISO 8601 week of a date, as <input type=week> writes it ("2026-W46").
    std::wstring IsoWeek(int y, int m, int d)
    {
        long long days = DaysFromCivil(y, m, d);
        int weekday = static_cast<int>(((days % 7) + 7 + 3) % 7); // 0 = Monday (1970-01-01 was a Thursday)
        long long thursday = days - weekday + 3;
        // The year the week belongs to is the year of its Thursday.
        int year = y;
        if (thursday < DaysFromCivil(y, 1, 1))
            year = y - 1;
        else if (thursday >= DaysFromCivil(y + 1, 1, 1))
            year = y + 1;
        int week = static_cast<int>((thursday - DaysFromCivil(year, 1, 1)) / 7) + 1;
        return FourDigits(year) + L"-W" + TwoDigits(week);
    }
}

void MainPage::AnswerPicker(const wchar_t* value)
{
    lw_view* view = m_pickerView;
    int id = m_pickerId;
    m_pickerView = nullptr;
    m_pickerId = 0;
    if (id && engine.pickerReply)
        engine.pickerReply(view, id, value);
}

Grid^ MainPage::AddPickerRow(const std::wstring& text, const std::wstring& detail, int swatch, bool enabled, bool selected,
                             std::function<void()> chosen)
{
    auto row = ref new Grid();
    row->Background = ref new SolidColorBrush(selected ? Windows::UI::ColorHelper::FromArgb(255, 0x19, 0x76, 0xD2)
                                                        : Windows::UI::ColorHelper::FromArgb(255, 0x1F, 0x1F, 0x1F));
    row->MinHeight = 56;
    auto first = ref new ColumnDefinition();
    first->Width = GridLength(1, GridUnitType::Auto);
    row->ColumnDefinitions->Append(first);
    auto second = ref new ColumnDefinition();
    second->Width = GridLength(1, GridUnitType::Star);
    row->ColumnDefinitions->Append(second);
    if (swatch >= 0)
    {
        auto square = ref new Border();
        square->Width = 40;
        square->Height = 40;
        square->Margin = Thickness(16, 8, 0, 8);
        square->BorderThickness = Thickness(1);
        square->BorderBrush = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x77, 0x77, 0x77));
        square->Background = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, static_cast<unsigned char>(swatch >> 16),
            static_cast<unsigned char>(swatch >> 8), static_cast<unsigned char>(swatch)));
        row->Children->Append(square);
    }
    auto block = ref new StackPanel();
    block->Margin = Thickness(16, 10, 12, 10);
    block->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
    Grid::SetColumn(block, 1);
    auto caption = ref new TextBlock();
    caption->Text = ref new String(text.c_str());
    caption->FontSize = 21;
    caption->TextWrapping = TextWrapping::Wrap;
    caption->MaxLines = 3;
    caption->Foreground = ref new SolidColorBrush(enabled ? Windows::UI::Colors::White : Windows::UI::ColorHelper::FromArgb(255, 0x77, 0x77, 0x77));
    block->Children->Append(caption);
    if (!detail.empty())
    {
        // Not "small": rpcndr.h defines it as char.
        auto detailBlock = ref new TextBlock();
        detailBlock->Text = ref new String(detail.c_str());
        detailBlock->FontSize = 14;
        detailBlock->Foreground = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0xAA, 0xAA, 0xAA));
        block->Children->Append(detailBlock);
    }
    row->Children->Append(block);
    if (enabled && chosen)
    {
        row->Tapped += ref new TappedEventHandler([chosen](Object^, TappedRoutedEventArgs^ e)
        {
            e->Handled = true;
            chosen();
        });
    }
    ListItems->Children->Append(row);
    return row;
}

void MainPage::ShowPicker(lw_view* view, int id, int kind, const std::wstring& text, const std::wstring& text2)
{
    if (kind == LW_PICKER_CLOSE)
    {
        // The page took its control away: nothing to answer.
        if (id == m_pickerId)
        {
            m_pickerId = 0;
            m_pickerView = nullptr;
            if (m_listKind == ListKind::Picker)
                HideList();
        }
        return;
    }
    if (m_pickerId)
        AnswerPicker(nullptr);
    if (view != m_view)
    {
        if (engine.pickerReply)
            engine.pickerReply(view, id, nullptr);
        return;
    }
    if (m_composeVisible)
        HideCompose(true, L"selettore");
    if (kind == LW_PICKER_DATE)
    {
        m_pickerView = view;
        m_pickerId = id;
        auto parts = Lines(text);
        ShowDatePicker(parts.empty() ? std::wstring(L"date") : parts[0], parts.size() > 1 ? parts[1] : std::wstring());
        return;
    }
    if (kind != LW_PICKER_SELECT && kind != LW_PICKER_COLOR)
    {
        if (engine.pickerReply)
            engine.pickerReply(view, id, nullptr);
        return;
    }
    // The list page; ShowList's own cleanup first (another list may be up), then this picker becomes the open one.
    if (m_listKind != ListKind::None)
        HideList();
    m_pickerView = view;
    m_pickerId = id;
    m_listKind = ListKind::Picker;
    ListItems->Children->Clear();
    ListTitle1->Text = kind == LW_PICKER_COLOR ? L"colore" : L"scegli";
    ListTitle1->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
    ListTitle2->Text = L"";
    ListActionText->Text = L"annulla";
    ListEmpty->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Grid^ selectedRow = nullptr;
    if (kind == LW_PICKER_SELECT)
    {
        auto items = Lines(text);
        for (size_t i = 0; i < items.size(); ++i)
        {
            wchar_t mark = items[i].empty() ? L' ' : items[i][0];
            std::wstring label = items[i].empty() ? std::wstring() : items[i].substr(1);
            if (mark == L'-')
            {
                auto line = ref new Border();
                line->Height = 2;
                line->Margin = Thickness(16, 6, 16, 6);
                line->Background = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x44, 0x44, 0x44));
                ListItems->Children->Append(line);
                continue;
            }
            if (mark == L'#')
            {
                auto header = ref new TextBlock();
                header->Text = ref new String(label.c_str());
                header->FontSize = 16;
                header->Margin = Thickness(16, 14, 12, 4);
                header->Foreground = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x90, 0xCA, 0xF9));
                ListItems->Children->Append(header);
                continue;
            }
            int index = static_cast<int>(i);
            auto row = AddPickerRow(label.empty() ? std::wstring(L" ") : label, std::wstring(), -1, mark != L'!', mark == L'*', [this, index]()
            {
                AnswerPicker(std::to_wstring(index).c_str());
                HideList();
            });
            if (mark == L'*')
                selectedRow = row;
        }
    }
    else
    {
        struct Named { const wchar_t* name; int color; };
        static const Named palette[] = {
            { L"nero", 0x000000 }, { L"grigio scuro", 0x444444 }, { L"grigio", 0x888888 }, { L"grigio chiaro", 0xCCCCCC },
            { L"bianco", 0xFFFFFF }, { L"rosso", 0xE51400 }, { L"cremisi", 0xA20025 }, { L"arancione", 0xFA6800 },
            { L"ambra", 0xF0A30A }, { L"giallo", 0xE3C800 }, { L"lime", 0xA4C400 }, { L"verde", 0x60A917 },
            { L"smeraldo", 0x008A00 }, { L"verde acqua", 0x00ABA9 }, { L"ciano", 0x1BA1E2 }, { L"cobalto", 0x0050EF },
            { L"indaco", 0x6A00FF }, { L"viola", 0xAA00FF }, { L"rosa", 0xF472D0 }, { L"magenta", 0xD80073 },
            { L"marrone", 0x825A2C }, { L"oliva", 0x6D8764 }, { L"acciaio", 0x647687 }, { L"malva", 0x76608A } };
        int current = ParseColor(text);
        auto add = [this, current, &selectedRow](const std::wstring& name, int color)
        {
            wchar_t hex[16];
            swprintf_s(hex, L"#%06x", color);
            std::wstring value = hex;
            auto row = AddPickerRow(name, value, color, true, color == current, [this, value]()
            {
                AnswerPicker(value.c_str());
                HideList();
            });
            if (color == current && !selectedRow)
                selectedRow = row;
        };
        // The page's own suggestions first, then the current colour if it is none of ours, then the palette.
        for (auto& suggested : Lines(text2))
        {
            int color = ParseColor(suggested);
            if (color >= 0)
                add(L"suggerito dalla pagina", color);
        }
        bool known = false;
        for (auto& entry : palette)
            known = known || entry.color == current;
        if (current >= 0 && !known && !selectedRow)
            add(L"colore attuale", current);
        for (auto& entry : palette)
            add(entry.name, entry.color);
    }
    ListScroll->ChangeView(nullptr, ref new Box<double>(0.0), nullptr, true);
    ListPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    // The chosen item in view, once the rows have their places.
    if (selectedRow)
    {
        Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([this, selectedRow, id]()
        {
            if (m_pickerId != id || m_listKind != ListKind::Picker)
                return;
            try
            {
                auto at = selectedRow->TransformToVisual(ListItems)->TransformPoint(Point(0, 0));
                ListScroll->ChangeView(nullptr, ref new Box<double>((std::max)(0.0, static_cast<double>(at.Y) - 120)), nullptr, true);
            }
            catch (Exception^)
            {
            }
        }));
    }
}

// The phone's date and time pickers. type: date, month and week take a date; time a time; datetime-local both, one
// after the other. A picker closed with the Back key answers "no choice".
void MainPage::ShowDatePicker(const std::wstring& type, const std::wstring& value)
{
    int id = m_pickerId;
    auto calendar = GregorianNow();
    int year = calendar->Year, month = calendar->Month, day = calendar->Day, hour = calendar->Hour, minute = calendar->Minute;
    // The input's current value, where it has one.
    if (type == L"time")
        swscanf_s(value.c_str(), L"%d:%d", &hour, &minute);
    else if (type == L"month")
        swscanf_s(value.c_str(), L"%d-%d", &year, &month);
    else if (type == L"week")
    {
        int week = 0, weekYear = 0;
        if (swscanf_s(value.c_str(), L"%d-W%d", &weekYear, &week) == 2 && week >= 1 && week <= 53)
        {
            // Monday of that ISO week: the week with January 4th is week 1.
            long long jan4 = DaysFromCivil(weekYear, 1, 4);
            int weekday = static_cast<int>(((jan4 % 7) + 7 + 3) % 7);
            long long monday = jan4 - weekday + (week - 1) * 7;
            calendar->Year = 1970;
            calendar->Month = 1;
            calendar->Day = 1;
            calendar->AddDays(static_cast<int>(monday));
            year = calendar->Year;
            month = calendar->Month;
            day = calendar->Day;
        }
    }
    else
        swscanf_s(value.c_str(), L"%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute);
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31)
    {
        calendar->SetToNow();
        year = calendar->Year;
        month = calendar->Month;
        day = calendar->Day;
    }
    hour = (std::min)((std::max)(hour, 0), 23);
    minute = (std::min)((std::max)(minute, 0), 59);

    auto askTime = [this, id, type, hour, minute](std::wstring datePart)
    {
        auto flyout = ref new TimePickerFlyout();
        TimeSpan start;
        start.Duration = (static_cast<long long>(hour) * 3600 + minute * 60) * 10000000LL;
        flyout->Time = start;
        flyout->ClockIdentifier = Windows::Globalization::ClockIdentifiers::TwentyFourHour;
        create_task(flyout->ShowAtAsync(this)).then([this, id, datePart](task<IBox<TimeSpan>^> done)
        {
            IBox<TimeSpan>^ picked = nullptr;
            try { picked = done.get(); } catch (Exception^) { }
            if (m_pickerId != id)
                return;
            if (!picked)
            {
                AnswerPicker(nullptr);
                return;
            }
            long long minutes = picked->Value.Duration / 600000000LL;
            std::wstring time = TwoDigits(static_cast<int>(minutes / 60 % 24)) + L":" + TwoDigits(static_cast<int>(minutes % 60));
            AnswerPicker((datePart.empty() ? time : datePart + L"T" + time).c_str());
        }, task_continuation_context::use_current());
    };
    if (type == L"time")
    {
        askTime(std::wstring());
        return;
    }
    auto flyout = ref new DatePickerFlyout();
    try
    {
        calendar->Year = year;
        calendar->Month = month;
        calendar->Day = (std::min)(day, calendar->NumberOfDaysInThisMonth);
        calendar->Hour = 12;
        flyout->Date = calendar->GetDateTime();
    }
    catch (Exception^)
    {
    }
    create_task(flyout->ShowAtAsync(this)).then([this, id, type, askTime](task<IBox<DateTime>^> done)
    {
        IBox<DateTime>^ picked = nullptr;
        try { picked = done.get(); } catch (Exception^) { }
        if (m_pickerId != id)
            return;
        if (!picked)
        {
            AnswerPicker(nullptr);
            return;
        }
        auto chosen = GregorianNow();
        chosen->SetDateTime(picked->Value);
        int y = chosen->Year, m = chosen->Month, d = chosen->Day;
        std::wstring date = FourDigits(y) + L"-" + TwoDigits(m) + L"-" + TwoDigits(d);
        if (type == L"month")
            AnswerPicker((FourDigits(y) + L"-" + TwoDigits(m)).c_str());
        else if (type == L"week")
            AnswerPicker(IsoWeek(y, m, d).c_str());
        else if (type == L"datetime-local")
            askTime(date);
        else
            AnswerPicker(date.c_str());
    }, task_continuation_context::use_current());
}

// Under the address while it is edited: favorites, then history, that contain what is typed (all of them while the
// box still shows the page's own address). The best match sits nearest to the box, at the bottom.
void MainPage::UpdateSuggestions()
{
    SuggestItems->Children->Clear();
    std::wstring typed = Lower(std::wstring(Address->Text->Data()));
    bool all = typed.empty() || typed == Lower(m_currentURL);
    std::vector<PageEntry> found;
    auto consider = [&](const PageEntry& entry)
    {
        if (found.size() >= 8)
            return;
        for (auto& other : found)
        {
            if (other.url == entry.url)
                return;
        }
        if (all || Lower(entry.url).find(typed) != std::wstring::npos || Lower(entry.title).find(typed) != std::wstring::npos)
            found.push_back(entry);
    };
    for (auto& entry : m_favorites)
        consider(entry);
    for (auto it = m_historyEntries.rbegin(); it != m_historyEntries.rend(); ++it)
        consider(*it);
    for (auto it = found.rbegin(); it != found.rend(); ++it)
    {
        std::wstring url = it->url;
        auto row = ref new Border();
        row->Background = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x1F, 0x1F, 0x1F));
        row->Padding = Thickness(16, 9, 16, 9);
        auto text = ref new StackPanel();
        auto caption = ref new TextBlock();
        caption->Text = ref new String(it->title.c_str());
        caption->FontSize = 19;
        caption->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
        caption->TextTrimming = TextTrimming::CharacterEllipsis;
        text->Children->Append(caption);
        auto detailBlock = ref new TextBlock();
        detailBlock->Text = ref new String(url.c_str());
        detailBlock->FontSize = 13;
        detailBlock->Foreground = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x64, 0xB5, 0xF6));
        detailBlock->TextTrimming = TextTrimming::CharacterEllipsis;
        text->Children->Append(detailBlock);
        row->Child = text;
        row->Tapped += ref new TappedEventHandler([this, url](Object^, TappedRoutedEventArgs^ e)
        {
            e->Handled = true;
            Navigate(url);
            HideKeyboard();
        });
        SuggestItems->Children->Append(row);
    }
    SuggestScroll->UpdateLayout();
    SuggestScroll->ChangeView(nullptr, ref new Box<double>(SuggestScroll->ScrollableHeight), nullptr, true);
}

// ---- dialogs ----

void MainPage::ShowPageDialog(lw_view* view, int id, int kind, const std::wstring& message, const std::wstring& value)
{
    // One at a time: a new one answers "cancel" to whatever was still open.
    if (DialogPanel->Visibility == Windows::UI::Xaml::Visibility::Visible)
        ReplaceDialog();
    auto tab = TabForView(view);
    m_dialogView = view;
    m_dialogId = id;
    m_dialogDone = nullptr;
    std::wstring host = tab ? HostOf(tab->url) : std::wstring();
    DialogTitle->Text = ref new String((host.empty() ? std::wstring(L"la pagina") : host).c_str());
    std::wstring text = message;
    const wchar_t* ok = L"ok";
    const wchar_t* cancel = L"annulla";
    m_dialogHasInput = kind == LW_DIALOG_PROMPT;
    if (kind == LW_DIALOG_BEFOREUNLOAD)
    {
        text = L"Vuoi lasciare questa pagina? Le modifiche che hai fatto potrebbero non essere salvate.";
        ok = L"esci";
        cancel = L"resta";
    }
    DialogText->Text = ref new String(text.c_str());
    DialogOkText->Text = ref new String(ok);
    DialogCancelText->Text = ref new String(cancel);
    DialogCancel->Visibility = kind == LW_DIALOG_ALERT ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    DialogInput->Visibility = m_dialogHasInput ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    DialogInput->Text = ref new String(value.c_str());
    DialogPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    if (m_dialogHasInput)
    {
        DialogInput->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        DialogInput->SelectAll();
    }
}

// A server asks for a name and a password (HTTP 401, LW_EVENT_PASSWORD): the dialog panel with both fields. The title is
// the server as the engine names it; the realm is the server's own text and is shown as such, in quotes. The load waits,
// the page does not: cancel (or Back) gives it the server's "unauthorized" answer.
void MainPage::ShowPasswordQuestion(lw_view* view, int id, int refused, const std::wstring& server, const std::wstring& realm)
{
    // Whatever was open is cancelled first (that also puts the password box away, if it was another question).
    if (DialogPanel->Visibility == Windows::UI::Xaml::Visibility::Visible)
        ReplaceDialog();
    m_dialogView = nullptr;
    m_dialogId = 0;
    m_dialogHasInput = true;
    DialogTitle->Text = ref new String(server.c_str());
    std::wstring text = refused ? L"Nome o password non accettati. Riprova." : L"Il sito chiede nome utente e password.";
    if (!realm.empty())
        text += L"\n\u00AB" + realm.substr(0, 120) + L"\u00BB";
    if (server.compare(0, 7, L"http://") == 0)
        text += L"\nLa connessione non \u00E8 cifrata: la password viaggia in chiaro.";
    DialogText->Text = ref new String(text.c_str());
    DialogOkText->Text = L"accedi";
    DialogCancelText->Text = L"annulla";
    DialogCancel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    DialogInput->Visibility = Windows::UI::Xaml::Visibility::Visible;
    DialogInput->Text = L"";
    DialogInput->PlaceholderText = L"nome utente";
    DialogPassword->Password = L"";
    DialogPassword->PlaceholderText = L"password";
    DialogPassword->Visibility = Windows::UI::Xaml::Visibility::Visible;
    m_dialogDone = [this, view, id](bool accepted, const std::wstring& user)
    {
        std::wstring password = DialogPassword->Password->Data();
        DialogPassword->Password = L"";
        DialogPassword->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        DialogInput->PlaceholderText = L"";
        // The engine finds the question by its id (the tab may be gone by now).
        if (engine.passwordReply)
            engine.passwordReply(view, id, accepted ? user.c_str() : nullptr, password.c_str());
    };
    DialogPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    DialogInput->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::ShowAppDialog(const std::wstring& title, const std::wstring& message, const std::wstring& value,
                             const wchar_t* ok, std::function<void(bool, const std::wstring&)> done)
{
    if (DialogPanel->Visibility == Windows::UI::Xaml::Visibility::Visible)
        ReplaceDialog();
    m_dialogView = nullptr;
    m_dialogId = 0;
    m_dialogDone = done;
    // A value to edit (a favorite's name) shows the text box; a plain question does not.
    m_dialogHasInput = !value.empty();
    DialogTitle->Text = ref new String(title.c_str());
    DialogText->Text = ref new String(message.c_str());
    DialogOkText->Text = ref new String(ok);
    DialogCancelText->Text = L"annulla";
    DialogCancel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    DialogInput->Visibility = m_dialogHasInput ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    DialogInput->Text = ref new String(value.c_str());
    DialogPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    if (m_dialogHasInput)
    {
        DialogInput->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        DialogInput->SelectAll();
    }
}

// A new dialog takes the place of the open one: that one is cancelled, and m_dialogReplaced tells its owner that the
// user did not answer it.
void MainPage::ReplaceDialog()
{
    m_dialogReplaced = true;
    CloseDialog(false);
    m_dialogReplaced = false;
}

void MainPage::CloseDialog(bool accepted)
{
    std::wstring value = m_dialogHasInput ? std::wstring(DialogInput->Text->Data()) : std::wstring();
    DialogPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (m_dialogHasInput)
        HideKeyboard();
    if (m_dialogView)
    {
        lw_view* view = m_dialogView;
        m_dialogView = nullptr;
        engine.dialogReply(view, m_dialogId, accepted ? 1 : 0, value.c_str());
    }
    if (m_dialogDone)
    {
        auto done = m_dialogDone;
        m_dialogDone = nullptr;
        done(accepted, value);
    }
}

void MainPage::DialogOk_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    CloseDialog(true);
}

void MainPage::DialogCancel_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    CloseDialog(false);
}

void MainPage::DialogInput_KeyUp(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter)
        return;
    e->Handled = true;
    // A password question: Enter on the name goes on to the password.
    if (DialogPassword->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        DialogPassword->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        return;
    }
    CloseDialog(true);
}

void MainPage::DialogPassword_KeyUp(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter)
        return;
    e->Handled = true;
    CloseDialog(true);
}

// ---- what pages ask of the phone ----

// A site wants the position or to show notifications (LW_EVENT_PERMISSION): the user says yes or no once, and the
// engine keeps the answer for that site until the app ends. Back is a no.
void MainPage::ShowPermissionQuestion(lw_view* view, int id, int kind, const std::wstring& site)
{
    if (!engine.permissionReply)
        return;
    const wchar_t* message = kind == LW_PERMISSION_GEOLOCATION ? L"Il sito vuole conoscere la posizione del telefono."
        : kind == LW_PERMISSION_NOTIFICATIONS ? L"Il sito vuole mostrare notifiche." : nullptr;
    if (!message)
    {
        engine.permissionReply(view, id, 0);
        return;
    }
    ShowAppDialog(site, message, L"", L"consenti", [this, view, id](bool accepted, const std::wstring&)
    {
        // Pushed away by another dialog: not an answer, the site may ask again.
        engine.permissionReply(view, id, m_dialogReplaced ? -1 : accepted ? 1 : 0);
    });
    DialogCancelText->Text = L"rifiuta";
}

namespace
{
    // Any thread (the Geolocator's events come on its own).
    void ReportPosition(Windows::Devices::Geolocation::Geoposition^ position)
    {
        if (!position || !engine.geolocationUpdate)
            return;
        auto coordinate = position->Coordinate;
        auto point = coordinate->Point->Position;
        double unknown = std::numeric_limits<double>::quiet_NaN();
        auto altitudeAccuracy = coordinate->AltitudeAccuracy;
        auto heading = coordinate->Heading;
        auto speed = coordinate->Speed;
        engine.geolocationUpdate(point.Latitude, point.Longitude, coordinate->Accuracy,
                                 altitudeAccuracy ? point.Altitude : unknown, altitudeAccuracy ? altitudeAccuracy->Value : unknown,
                                 heading ? heading->Value : unknown, speed ? speed->Value : unknown);
    }
}

// LW_EVENT_GEOLOCATION: the position source runs while a page watches (1 = network accuracy, 2 = GPS) and stops at 0.
// The user's yes came through ShowPermissionQuestion; the phone's own location switch is the Geolocator's to check.
void MainPage::WatchPosition(int accuracy)
{
    using namespace Windows::Devices::Geolocation;
    if (m_geolocator)
    {
        m_geolocator->PositionChanged -= m_positionToken;
        m_geolocator->StatusChanged -= m_positionStatusToken;
        m_geolocator = nullptr;
    }
    if (!accuracy || !engine.geolocationUpdate || !engine.geolocationError)
        return;
    try
    {
        m_geolocator = ref new Geolocator();
        m_geolocator->DesiredAccuracy = accuracy == 2 ? PositionAccuracy::High : PositionAccuracy::Default;
        m_geolocator->ReportInterval = accuracy == 2 ? 1000 : 5000;
        m_positionToken = m_geolocator->PositionChanged += ref new TypedEventHandler<Geolocator^, PositionChangedEventArgs^>(
            [](Geolocator^, PositionChangedEventArgs^ e)
        {
            ReportPosition(e->Position);
        });
        m_positionStatusToken = m_geolocator->StatusChanged += ref new TypedEventHandler<Geolocator^, StatusChangedEventArgs^>(
            [](Geolocator^, StatusChangedEventArgs^ e)
        {
            if (e->Status == PositionStatus::Disabled)
                engine.geolocationError(1);
            else if (e->Status == PositionStatus::NotAvailable)
                engine.geolocationError(2);
        });
        // The first position at once (one the phone already has will do), not at the first report.
        TimeSpan maximumAge;
        maximumAge.Duration = 60 * 10000000LL;
        TimeSpan timeout;
        timeout.Duration = 30 * 10000000LL;
        create_task(m_geolocator->GetGeopositionAsync(maximumAge, timeout)).then([](task<Geoposition^> t)
        {
            try
            {
                ReportPosition(t.get());
            }
            catch (AccessDeniedException^)
            {
                engine.geolocationError(1);
            }
            catch (Exception^)
            {
                engine.geolocationError(2);
            }
            catch (...)
            {
            }
        });
    }
    catch (Exception^ e)
    {
        Log(L"posizione: " + std::wstring(e->Message->Data()));
        m_geolocator = nullptr;
        engine.geolocationError(1);
    }
}

// LW_EVENT_NOTIFICATION: the page's notification as a toast of the phone (it stays in the notification centre); a tap
// on it is the page's click event. show = false: the page took it back.
void MainPage::ShowPageNotification(lw_view* view, int id, bool show, const std::wstring& title, const std::wstring& body)
{
    using namespace Windows::UI::Notifications;
    try
    {
        auto notifier = ToastNotificationManager::CreateToastNotifier();
        if (!show)
        {
            auto found = m_toasts.find(id);
            if (found != m_toasts.end())
            {
                notifier->Hide(found->second);
                m_toasts.erase(found);
            }
            return;
        }
        auto xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        auto lines = xml->GetElementsByTagName(L"text");
        lines->Item(0)->AppendChild(xml->CreateTextNode(ref new String(title.substr(0, 100).c_str())));
        lines->Item(1)->AppendChild(xml->CreateTextNode(ref new String(body.substr(0, 300).c_str())));
        auto toast = ref new ToastNotification(xml);
        toast->Activated += ref new TypedEventHandler<ToastNotification^, Object^>([view, id](ToastNotification^, Object^)
        {
            // The engine finds the notification by its id (the tab may be gone).
            if (engine.notificationClicked)
                engine.notificationClicked(view, id);
        });
        notifier->Show(toast);
        // The last few stay at hand for the page to take back.
        if (m_toasts.size() >= 16)
            m_toasts.erase(m_toasts.begin());
        m_toasts[id] = toast;
    }
    catch (Exception^ e)
    {
        Log(L"notifica: " + std::wstring(e->Message->Data()));
        if (show)
            ShowNotice(body.empty() ? title : title + L": " + body, nullptr);
    }
}

// LW_EVENT_FULLSCREEN: the page takes the whole screen (view = the tab that asked) or gives it back (nullptr). The bar
// and the phone's status bar go; Pages grows, and the views follow its size as for any resize.
void MainPage::ShowFullscreen(lw_view* view)
{
    bool was = m_fullscreenView != nullptr;
    m_fullscreenView = view;
    if (was == (view != nullptr))
        return;
    Bar->Visibility = view ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    try
    {
        auto statusBar = Windows::UI::ViewManagement::StatusBar::GetForCurrentView();
        if (view)
            statusBar->HideAsync();
        else
            statusBar->ShowAsync();
    }
    catch (Exception^)
    {
    }
    if (view)
        ShowNotice(L"schermo intero: Indietro per uscire", nullptr);
}

// ---- find in page ----

void MainPage::ShowFind()
{
    if (!m_view || !engine.find)
        return;
    m_findMatches = 0;
    m_findIndex = 0;
    FindCount->Text = L"";
    FindText->Text = L"";
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
    FindText->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::HideFind()
{
    if (FindBar->Visibility == Windows::UI::Xaml::Visibility::Collapsed)
        return;
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (m_view && engine.find)
        engine.find(m_view, ++m_findId, L"", 0);
    HideKeyboard();
}

// direction 0: the text changed (the first match from the current one on), 1 next, -1 previous.
void MainPage::Find(int direction)
{
    if (!m_view || !engine.find)
        return;
    std::wstring text = FindText->Text->Data();
    m_findDirection = direction;
    engine.find(m_view, ++m_findId, text.c_str(), direction < 0 ? 1 : 0);
}

void MainPage::FindText_TextChanged(Object^, TextChangedEventArgs^)
{
    Find(0);
}

void MainPage::FindText_KeyUp(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter)
        return;
    e->Handled = true;
    Find(1);
}

void MainPage::FindPrevious_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    Find(-1);
}

void MainPage::FindNext_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    Find(1);
}

void MainPage::FindClose_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    HideFind();
}

// ---- mobile or desktop version of a site ----

bool MainPage::IsDesktopSite(const std::wstring& url)
{
    std::wstring site = SiteOf(url);
    for (auto& other : m_desktopSites)
    {
        if (other == site)
            return true;
    }
    std::wstring host = HostOf(url);
    for (auto always : alwaysDesktopSites)
    {
        if (host == always)
            return true;
    }
    return false;
}

// "ua <name>": one site (and its subdomains) sees another browser, remembered across runs; "ua iphone" is the default
// again. The page loads again with it.
void MainPage::SetSiteIdentity(const std::wstring& url, const std::wstring& name)
{
    std::wstring site = SiteOf(url);
    if (site.empty() || !engine.setSiteUserAgent)
        return;
    std::wstring current = L"iphone";
    for (auto& entry : m_siteIdentities)
    {
        if (entry.first == site)
            current = entry.second;
    }
    bool known = false;
    for (auto& identity : identities)
        known = known || name == identity.name;
    if (!known)
    {
        std::wstring names;
        for (auto& identity : identities)
            names += (names.empty() ? L"" : L", ") + std::wstring(identity.name);
        ShowNotice(site + L" vede: " + current + L". Scrivi \u201Cua\u201D seguito da uno tra: " + names + L".", nullptr);
        return;
    }
    m_siteIdentities.erase(std::remove_if(m_siteIdentities.begin(), m_siteIdentities.end(),
        [&site](const std::pair<std::wstring, std::wstring>& entry) { return entry.first == site; }), m_siteIdentities.end());
    if (name != L"iphone")
        m_siteIdentities.push_back(std::make_pair(site, name));
    engine.setSiteUserAgent(site.c_str(), IdentityAgent(name));
    std::wstring list;
    for (auto& entry : m_siteIdentities)
        list += (list.empty() ? L"" : L"\n") + entry.first + L"\t" + entry.second;
    ApplicationData::Current->LocalSettings->Values->Insert(L"siteIdentities", ref new String(list.c_str()));
    Log(L"identita' per " + site + L": " + name);
    ShowNotice(site + L" ora vede: " + name + L". Ricarico la pagina.", nullptr);
    if (m_view)
        engine.loadURL(m_view, url.c_str());
}

void MainPage::ToggleDesktopSite(const std::wstring& url)
{
    std::wstring site = SiteOf(url);
    if (site.empty() || !engine.setSiteUserAgent)
        return;
    bool desktop = !IsDesktopSite(url);
    m_desktopSites.erase(std::remove(m_desktopSites.begin(), m_desktopSites.end(), site), m_desktopSites.end());
    if (desktop)
        m_desktopSites.push_back(site);
    engine.setSiteUserAgent(site.c_str(), desktop ? desktopUserAgent : nullptr);
    std::wstring list;
    for (auto& other : m_desktopSites)
        list += (list.empty() ? L"" : L"\n") + other;
    ApplicationData::Current->LocalSettings->Values->Insert(L"desktopSites", ref new String(list.c_str()));
    Log(std::wstring(L"versione ") + (desktop ? L"desktop" : L"mobile") + L" per " + site);
    // The site's address for phones (m.youtube.com, it.m.wikipedia.org) would stay mobile: going to desktop drops its
    // "m." label; going back to mobile the site redirects by itself.
    std::wstring target = url;
    if (desktop)
    {
        size_t scheme = url.find(L"://");
        size_t mobile = scheme == std::wstring::npos ? std::wstring::npos : url.find(L"m.", scheme + 3);
        size_t hostEnd = scheme == std::wstring::npos ? std::wstring::npos : url.find_first_of(L"/?#:", scheme + 3);
        if (mobile != std::wstring::npos && mobile < hostEnd && (mobile == scheme + 3 || url[mobile - 1] == L'.'))
            target = url.substr(0, mobile) + url.substr(mobile + 2);
    }
    if (m_view)
        engine.loadURL(m_view, target.c_str());
}

// ---- tiles ----

// A tile on Start for this page: its address is the tile's argument, which the app opens when launched from it. The icon
// is chosen here. A site's icon keeps its own colours on a background of its own; our icons are white on a transparent
// tile, which the phone paints in the accent colour. On a page that already has its tile, the choice changes that tile.
namespace
{
    const int iconScriptId = 0x1C0;
    const unsigned int tileIconSize = 360;     // 150 at the 1020's scale 240
    const unsigned int siteIconSize = 216;     // inside the tile; a glyph of ours is 144
    const size_t maxSiteIcons = 4;

    // tools/icons, TILE_ICONS.
    const wchar_t* const TileIcons[] = { L"globe", L"mail-unread", L"chat-message-new", L"x-office-calendar", L"x-office-document",
        L"document-edit", L"folder", L"image-x-generic", L"camera-photo", L"video-x-generic", L"audio-x-generic",
        L"media-playback-start", L"tv", L"weather-few-clouds", L"emoji-travel", L"emoji-food", L"emoji-nature", L"emoji-people",
        L"user-available", L"starred", L"channel-secure", L"dialog-password", L"phone", L"printer", L"network-wireless",
        L"system-search", L"applications-games", L"applications-science", L"utilities-terminal" };

    // The page's icons, biggest declared first, then the two places every browser tries.
    const wchar_t* const iconLinksScript =
        L"(function(){var l=[].slice.call(document.querySelectorAll('link[rel~=\"icon\"],link[rel=\"apple-touch-icon\"],"
        L"link[rel=\"apple-touch-icon-precomposed\"]'));"
        L"function s(e){var m=/(\\d+)x\\d+/.exec(e.getAttribute('sizes')||'');return m?+m[1]:(/apple/.test(e.rel)?180:16)}"
        L"l.sort(function(a,b){return s(b)-s(a)});"
        L"var u=l.map(function(e){return e.href});u.push(location.origin+'/apple-touch-icon.png',location.origin+'/favicon.ico');"
        L"return u.join('\\n')})()";

    std::wstring PinTileId(const std::wstring& url)
    {
        unsigned int hash = 2166136261u;
        for (wchar_t c : url)
        {
            hash ^= static_cast<unsigned int>(c);
            hash *= 16777619u;
        }
        return L"pagina" + std::to_wstring(hash);
    }

    Brush^ AccentBrush()
    {
        auto resources = Application::Current->Resources;
        if (resources->HasKey(L"PhoneAccentBrush"))
            return safe_cast<Brush^>(resources->Lookup(L"PhoneAccentBrush"));
        return ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x19, 0x76, 0xD2));
    }

    Border^ IconChoice(Brush^ background, Uri^ image)
    {
        auto choice = ref new Border();
        choice->Background = background;
        choice->Width = 110;
        choice->Height = 110;
        auto picture = ref new Image();
        picture->Source = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage(image);
        picture->Stretch = Stretch::Uniform;
        choice->Child = picture;
        return choice;
    }

    // The tile's picture: the icon centred on a background of its own, the colour of its corners if it fills them, else
    // white (a logo with a transparent background is drawn for a light page).
    Windows::UI::Color ComposeSiteIcon(const uint8_t* source, unsigned int width, unsigned int height, std::vector<uint8_t>& tile)
    {
        unsigned int red = 0, green = 0, blue = 0, opaque = 0;
        const unsigned int cornersX[] = { 0, width - 1, 0, width - 1 };
        const unsigned int cornersY[] = { 0, 0, height - 1, height - 1 };
        for (int i = 0; i < 4; ++i)
        {
            const uint8_t* pixel = source + (cornersY[i] * width + cornersX[i]) * 4;
            if (pixel[3] >= 200)
            {
                blue += pixel[0];
                green += pixel[1];
                red += pixel[2];
                opaque++;
            }
        }
        Windows::UI::Color background = opaque == 4
            ? Windows::UI::ColorHelper::FromArgb(255, static_cast<uint8_t>(red / 4), static_cast<uint8_t>(green / 4), static_cast<uint8_t>(blue / 4))
            : Windows::UI::Colors::White;
        tile.resize(tileIconSize * tileIconSize * 4);
        for (size_t i = 0; i < tile.size(); i += 4)
        {
            tile[i] = background.B;
            tile[i + 1] = background.G;
            tile[i + 2] = background.R;
            tile[i + 3] = 255;
        }
        unsigned int left = (tileIconSize - width) / 2, top = (tileIconSize - height) / 2;
        for (unsigned int y = 0; y < height; ++y)
        {
            for (unsigned int x = 0; x < width; ++x)
            {
                const uint8_t* from = source + (y * width + x) * 4;
                uint8_t* to = tile.data() + ((y + top) * tileIconSize + x + left) * 4;
                unsigned int alpha = from[3];
                for (int c = 0; c < 3; ++c)
                    to[c] = static_cast<uint8_t>((from[c] * alpha + to[c] * (255 - alpha)) / 255);
            }
        }
        return background;
    }
}

void MainPage::PinToStart(const std::wstring& url, const std::wstring& title)
{
    m_pinURL = url;
    m_pinId = PinTileId(url);
    m_pinName = title.empty() || title == url ? HostOf(url) : title;
    if (m_pinName.size() > 40)
        m_pinName = m_pinName.substr(0, 40);
    ++m_iconGeneration;
    m_siteIcons = 0;
    IconPickerTitle->Text = ref new String(m_pinName.c_str());
    IconSiteLabel->Text = L"cerco l'icona del sito...";
    IconSiteGrid->Children->Clear();
    if (IconPresetGrid->Children->Size == 0)
    {
        for (auto name : TileIcons)
        {
            std::wstring logo = std::wstring(L"ms-appx:///Assets/TileIcons/") + name + L".png";
            auto choice = IconChoice(AccentBrush(), ref new Uri(ref new String(logo.c_str())));
            choice->Tapped += ref new TappedEventHandler([this, logo](Object^, TappedRoutedEventArgs^)
            {
                ApplyTile(logo, 0);
            });
            IconPresetGrid->Children->Append(choice);
        }
    }
    IconPicker->Visibility = Windows::UI::Xaml::Visibility::Visible;
    if (m_view && engine.runScript)
        engine.runScript(m_view, iconScriptId, iconLinksScript);
    else
        IconSiteLabel->Text = L"icona del sito non trovata";
}

void MainPage::OnSiteIconLinks(const std::wstring& text)
{
    auto links = std::make_shared<std::vector<std::wstring>>();
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring link = text.substr(start, end - start);
        if (link.compare(0, 4, L"http") == 0 && std::find(links->begin(), links->end(), link) == links->end())
            links->push_back(link);
        start = end + 1;
    }
    Log(L"tile: " + std::to_wstring(links->size()) + L" icone candidate");
    LoadSiteIcons(links, 0, m_iconGeneration);
}

// One link at a time: fetched, decoded (the biggest image of an .ico), scaled and composed into the tile's PNG. What
// WIC cannot read (SVG) or the server refuses is skipped.
void MainPage::LoadSiteIcons(std::shared_ptr<std::vector<std::wstring>> links, size_t next, int generation)
{
    using namespace Windows::Graphics::Imaging;
    using namespace Windows::Storage::Streams;
    if (generation != m_iconGeneration)
        return;
    if (next >= links->size() || m_siteIcons >= static_cast<int>(maxSiteIcons))
    {
        IconSiteLabel->Text = m_siteIcons ? L"icona del sito" : L"icona del sito non trovata";
        return;
    }
    auto client = ref new HttpClient();
    auto stream = ref new InMemoryRandomAccessStream();
    auto decoded = std::make_shared<BitmapDecoder^>(nullptr);
    auto background = std::make_shared<Windows::UI::Color>();
    auto id = m_pinId;
    auto index = next;
    std::wstring name = id + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(next) + L".png";
    create_task(client->GetBufferAsync(ref new Uri(ref new String((*links)[next].c_str()))))
        .then([stream](IBuffer^ body)
    {
        return create_task(stream->WriteAsync(body));
    }).then([stream](unsigned int)
    {
        stream->Seek(0);
        return create_task(BitmapDecoder::CreateAsync(stream));
    }).then([decoded](BitmapDecoder^ decoder)
    {
        *decoded = decoder;
        // An .ico holds several sizes: the biggest.
        std::vector<task<BitmapFrame^>> frames;
        for (unsigned int i = 0; i < decoder->FrameCount; ++i)
            frames.push_back(create_task(decoder->GetFrameAsync(i)));
        return when_all(frames.begin(), frames.end());
    }).then([](std::vector<BitmapFrame^> frames)
    {
        BitmapFrame^ best = frames.at(0);
        for (auto frame : frames)
        {
            if (frame->PixelWidth > best->PixelWidth)
                best = frame;
        }
        if (best->PixelWidth < 32)
            throw Exception::CreateException(E_FAIL);
        unsigned int longest = std::max(best->PixelWidth, best->PixelHeight);
        unsigned int target = siteIconSize;
        auto transform = ref new BitmapTransform();
        transform->ScaledWidth = best->PixelWidth * target / longest;
        transform->ScaledHeight = best->PixelHeight * target / longest;
        transform->InterpolationMode = BitmapInterpolationMode::Fant;
        auto width = transform->ScaledWidth, height = transform->ScaledHeight;
        return create_task(best->GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight, transform,
            ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage))
            .then([width, height](PixelDataProvider^ pixels)
        {
            auto data = pixels->DetachPixelData();
            auto tile = std::make_shared<std::vector<uint8_t>>();
            Windows::UI::Color color = ComposeSiteIcon(data->Data, width, height, *tile);
            return std::make_pair(tile, color);
        });
    }).then([background, name](std::pair<std::shared_ptr<std::vector<uint8_t>>, Windows::UI::Color> composed)
    {
        *background = composed.second;
        auto tile = composed.first;
        return create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"Tiles", CreationCollisionOption::OpenIfExists))
            .then([name](StorageFolder^ folder)
        {
            return create_task(folder->CreateFileAsync(ref new String(name.c_str()), CreationCollisionOption::ReplaceExisting));
        }).then([](StorageFile^ file)
        {
            return create_task(file->OpenAsync(FileAccessMode::ReadWrite));
        }).then([tile](IRandomAccessStream^ output)
        {
            return create_task(BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId, output)).then([tile, output](BitmapEncoder^ encoder)
            {
                encoder->SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, tileIconSize, tileIconSize, 96, 96,
                    ArrayReference<uint8_t>(tile->data(), static_cast<unsigned int>(tile->size())));
                return create_task(encoder->FlushAsync());
            });
        });
    }).then([this, links, index, generation, name, background](task<void> finished)
    {
        try
        {
            finished.get();
            if (generation == m_iconGeneration)
                AddSiteIconChoice(name, (255u << 24) | (background->R << 16) | (background->G << 8) | background->B);
        }
        catch (Exception^ e)
        {
            Log(L"tile: icona " + std::to_wstring(index) + L" scartata: 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)));
        }
        catch (std::exception&)
        {
            Log(L"tile: icona " + std::to_wstring(index) + L" scartata");
        }
        LoadSiteIcons(links, index + 1, generation);
    }, task_continuation_context::use_current());
}

void MainPage::AddSiteIconChoice(const std::wstring& file, unsigned int background)
{
    std::wstring logo = L"ms-appdata:///local/Tiles/" + file;
    auto color = Windows::UI::ColorHelper::FromArgb(255, (background >> 16) & 255, (background >> 8) & 255, background & 255);
    auto choice = IconChoice(ref new SolidColorBrush(color), ref new Uri(ref new String(logo.c_str())));
    choice->Tapped += ref new TappedEventHandler([this, logo, background](Object^, TappedRoutedEventArgs^)
    {
        ApplyTile(logo, background);
    });
    IconSiteGrid->Children->Append(choice);
    m_siteIcons++;
}

void MainPage::ApplyTile(const std::wstring& logoPath, unsigned int argb)
{
    using namespace Windows::UI::StartScreen;
    CloseIconPicker();
    try
    {
        auto logo = ref new Uri(ref new String(logoPath.c_str()));
        auto background = Windows::UI::ColorHelper::FromArgb(argb >> 24, (argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
        auto id = ref new String(m_pinId.c_str());
        bool exists = SecondaryTile::Exists(id);
        auto tile = exists ? ref new SecondaryTile(id)
            : ref new SecondaryTile(id, ref new String(m_pinName.c_str()), ref new String(m_pinURL.c_str()), logo, TileSize::Square150x150);
        tile->VisualElements->Square150x150Logo = logo;
        tile->VisualElements->Square71x71Logo = logo;
        tile->VisualElements->BackgroundColor = background;
        tile->VisualElements->ShowNameOnSquare150x150Logo = true;
        // Words over the site's colour stay readable; a transparent tile is the accent colour, as the system's.
        bool lightTile = background.A == 255 && (background.R * 299 + background.G * 587 + background.B * 114) / 1000 > 160;
        tile->VisualElements->ForegroundText = lightTile ? ForegroundText::Dark : ForegroundText::Light;
        auto request = exists ? tile->UpdateAsync() : tile->RequestCreateAsync();
        create_task(request).then([](task<bool> t)
        {
            try { t.get(); } catch (Exception^) { }
        });
        Log((exists ? L"tile aggiornato: " : L"tile: ") + m_pinName);
    }
    catch (Exception^ e)
    {
        Log(L"tile non creato: 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)));
    }
}

void MainPage::CloseIconPicker()
{
    ++m_iconGeneration;
    IconPicker->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::IconPickerClose_Tapped(Object^, TappedRoutedEventArgs^)
{
    CloseIconPicker();
}

// ---- downloads ----
// The engine writes each file straight into Downloads\Internet (lw_config.download_directory) and reports it
// (LW_EVENT_DOWNLOAD, _PROGRESS, _FINISHED, also after its tab is gone). The list page shows this run's downloads and
// the files earlier runs left there. Opening or sharing goes through a copy in LocalFolder: the system's launcher and
// share target only take files the app's own storage holds.

namespace
{
    std::wstring FileNameOf(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    std::wstring ExtensionOf(const std::wstring& path)
    {
        std::wstring name = FileNameOf(path);
        size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos ? std::wstring() : Lower(name.substr(dot));
    }

    bool HasExtension(const std::wstring& path, const wchar_t* const* list)
    {
        std::wstring extension = ExtensionOf(path);
        for (; *list; ++list)
        {
            if (extension == *list)
                return true;
        }
        return false;
    }

    const wchar_t* const imageExtensions[] = { L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", nullptr };
    const wchar_t* const audioExtensions[] = { L".mp3", L".m4a", L".aac", L".wma", L".wav", L".amr", L".ogg", L".opus", L".flac", nullptr };
    const wchar_t* const videoExtensions[] = { L".mp4", L".3gp", L".wmv", L".mov", L".avi", L".mkv", L".webm", nullptr };

    std::wstring SizeText(long long bytes)
    {
        wchar_t text[32];
        if (bytes < 1024 * 1024)
            swprintf_s(text, L"%lld KB", (bytes + 1023) / 1024);
        else
            swprintf_s(text, L"%.1f MB", bytes / 1048576.0);
        return text;
    }

    // GetFileAttributesW is not in the phone's app API; the Ex form is.
    bool Exists(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
    }

    long long FileSize(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return -1;
        return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }

    // `name` in `folder`, with " (1)", " (2)"... while the name is taken.
    std::wstring FreePath(const std::wstring& folder, const std::wstring& name)
    {
        size_t dot = name.find_last_of(L'.');
        std::wstring stem = dot == std::wstring::npos || !dot ? name : name.substr(0, dot);
        std::wstring extension = dot == std::wstring::npos || !dot ? std::wstring() : name.substr(dot);
        std::wstring path = folder + L"\\" + name;
        for (int number = 1; Exists(path) && number < 1000; ++number)
            path = folder + L"\\" + stem + L" (" + std::to_wstring(number) + L")" + extension;
        return path;
    }

    // A copy in LocalFolder\Open (replacing the last one of that name), for the launcher and the share target. On a
    // thread-pool thread: a video takes seconds to copy.
    task<StorageFile^> LocalCopy(const std::wstring& path)
    {
        std::wstring folder = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\Open";
        return create_task([path, folder]()
        {
            CreateDirectoryW(folder.c_str(), nullptr);
            std::wstring copy = folder + L"\\" + FileNameOf(path);
            HRESULT result = CopyFile2(path.c_str(), copy.c_str(), nullptr);
            if (FAILED(result))
                throw Exception::CreateException(result);
            return copy;
        }).then([](std::wstring copy)
        {
            return create_task(StorageFile::GetFileFromPathAsync(ref new String(copy.c_str())));
        });
    }
}

void MainPage::OnDownloadEvent(int type, long long number, int id, const std::wstring& text, const std::wstring& text2)
{
    LoadDownloads();
    DownloadEntry* entry = nullptr;
    for (auto& other : m_downloads)
    {
        if (other.id == id)
            entry = &other;
    }
    if (type == LW_EVENT_DOWNLOAD)
    {
        if (!entry)
        {
            m_downloads.push_back(DownloadEntry());
            entry = &m_downloads.back();
        }
        entry->id = id;
        // A data: address can be megabytes long; only "riprova" would use it, and it cannot.
        entry->url = text.compare(0, 5, L"data:") == 0 ? std::wstring(L"data:") : text;
        entry->path = text2;
        entry->name = FileNameOf(text2.empty() ? text : text2);
        entry->total = number;
        entry->state = 0;
        // "Salva immagine" asked for this address (or, after a redirect, for the only image still awaited).
        auto asked = std::find(m_imageSaves.begin(), m_imageSaves.end(), text);
        if (asked == m_imageSaves.end() && m_imageSaves.size() == 1)
            asked = m_imageSaves.begin();
        if (asked != m_imageSaves.end())
        {
            entry->toPictures = true;
            m_imageSaves.erase(asked);
        }
        if (!text2.empty() && !entry->toPictures)
            ShowNotice(L"Download di " + entry->name + L" iniziato. Tocca per vedere i download.", [this]() { ShowList(ListKind::Downloads); });
    }
    else if (!entry)
        return;
    else if (type == LW_EVENT_DOWNLOAD_PROGRESS)
    {
        entry->received = number;
        // The list follows the bytes at most once a second.
        if (m_listKind != ListKind::Downloads || GetTickCount64() - m_downloadsRefreshed < 1000)
            return;
    }
    else if (type == LW_EVENT_DOWNLOAD_FINISHED)
    {
        entry->state = number == 0 ? 1 : number == 1 ? 2 : 3;
        entry->error = text2;
        if (entry->state == 1)
        {
            entry->received = std::max(FileSize(entry->path), 0LL);
            std::wstring path = entry->path;
            if (entry->toPictures)
            {
                // Out of Downloads into the phone's pictures, where the gallery shows it.
                std::wstring folder = L"C:\\Data\\Users\\Public\\Pictures\\Saved Pictures";
                CreateDirectoryW(folder.c_str(), nullptr);
                std::wstring target = FreePath(folder, entry->name);
                if (MoveFileExW(path.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED))
                {
                    entry->path = target;
                    entry->name = FileNameOf(target);
                    path = target;
                    ShowNotice(L"Immagine salvata in Immagini salvate. Tocca per aprirla.", [this, path]() { OpenFile(path); });
                }
                else
                    ShowNotice(L"Immagine scaricata in Download\\Internet. Tocca per aprirla.", [this, path]() { OpenFile(path); });
            }
            else
                ShowNotice(entry->name + L" scaricato. Tocca per aprirlo.", [this, path]() { OpenFile(path); });
        }
        else if (entry->state == 2)
            ShowNotice(L"Download di " + entry->name + L" non riuscito" + (text2.empty() ? L"." : L": " + text2 + L"."),
                [this]() { ShowList(ListKind::Downloads); });
        Log(L"download " + std::to_wstring(id) + (entry->state == 1 ? L" completato: " : entry->state == 2 ? L" non riuscito: " : L" annullato: ")
            + entry->path);
    }
    if (m_listKind == ListKind::Downloads)
        RefreshDownloads();
}

// The files earlier runs left in Downloads\Internet, oldest first, before this run's.
void MainPage::LoadDownloads()
{
    if (m_downloadsLoaded)
        return;
    m_downloadsLoaded = true;
    struct Found { std::wstring path; long long size; unsigned long long time; };
    std::vector<Found> found;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileExW((std::wstring(downloadFolder) + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN))
                continue;
            Found file;
            file.path = std::wstring(downloadFolder) + L"\\" + data.cFileName;
            file.size = (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            file.time = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            found.push_back(file);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.time < b.time; });
    std::vector<DownloadEntry> earlier;
    for (auto& file : found)
    {
        bool known = false;
        for (auto& entry : m_downloads)
            known = known || _wcsicmp(entry.path.c_str(), file.path.c_str()) == 0;
        if (known)
            continue;
        DownloadEntry entry;
        entry.path = file.path;
        entry.name = FileNameOf(file.path);
        entry.received = file.size;
        entry.total = file.size;
        entry.state = 1;
        earlier.push_back(entry);
    }
    m_downloads.insert(m_downloads.begin(), earlier.begin(), earlier.end());
}

// The list again, at the same scroll position (progress, a download finished).
void MainPage::RefreshDownloads()
{
    if (m_listKind != ListKind::Downloads || ListPanel->Visibility != Windows::UI::Xaml::Visibility::Visible)
        return;
    double offset = ListScroll->VerticalOffset;
    ShowList(ListKind::Downloads);
    ListScroll->ChangeView(nullptr, ref new Box<double>(offset), nullptr, true);
}

void MainPage::ShowDownloadMenu(const DownloadEntry& entry)
{
    MenuItems->Children->Clear();
    MenuTitle->Text = ref new String(entry.name.c_str());
    if (entry.state == 0)
    {
        int id = entry.id;
        AddMenuItem(L"annulla download", [this, id]() { if (engine.downloadReply) engine.downloadReply(nullptr, id, nullptr); });
    }
    else if (entry.state == 1)
    {
        std::wstring path = entry.path;
        AddMenuItem(L"apri", [this, path]() { OpenFile(path); });
        AddMenuItem(L"condividi", [this, path]() { ShareFile(path); });
        if (HasExtension(path, imageExtensions) && path.find(L"\\Pictures\\") == std::wstring::npos)
            AddMenuItem(L"copia nelle immagini", [this, path]() { CopyToLibrary(path, L"C:\\Data\\Users\\Public\\Pictures\\Saved Pictures", L"Immagini salvate"); });
        if (HasExtension(path, audioExtensions))
            AddMenuItem(L"copia nella musica", [this, path]() { CopyToLibrary(path, L"C:\\Data\\Users\\Public\\Music\\Internet", L"Musica"); });
        if (HasExtension(path, videoExtensions))
            AddMenuItem(L"copia nei video", [this, path]() { CopyToLibrary(path, L"C:\\Data\\Users\\Public\\Videos\\Internet", L"Video"); });
        AddMenuItem(L"elimina file", [this, entry]() { RemoveDownload(entry, true); });
    }
    else
    {
        std::wstring url = entry.url;
        if (url.compare(0, 4, L"http") == 0)
            AddMenuItem(L"riprova", [this, url]() { HideList(); Navigate(url); });
        AddMenuItem(L"togli dall'elenco", [this, entry]() { RemoveDownload(entry, false); });
    }
    Menu->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

// With the app the phone has for its type (PDF reader, Office, Musica, Foto...).
void MainPage::OpenFile(const std::wstring& path)
{
    // The launcher only works from the UI thread.
    LocalCopy(path).then([](StorageFile^ file)
    {
        return create_task(Windows::System::Launcher::LaunchFileAsync(file));
    }, task_continuation_context::use_current()).then([this, path](task<bool> launched)
    {
        bool ok = false;
        try
        {
            ok = launched.get();
        }
        catch (Exception^ e)
        {
            Log(L"apri file: errore 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)) + L" " + path);
        }
        if (!ok)
            ShowNotice(L"Nessuna app apre " + FileNameOf(path) + L". Il file \u00E8 in Download\\Internet (app File).", nullptr);
    }, task_continuation_context::use_current());
}

void MainPage::ShareFile(const std::wstring& path)
{
    LocalCopy(path).then([this, path](task<StorageFile^> copied)
    {
        try
        {
            m_shareFile = copied.get()->Path->Data();
            Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI();
        }
        catch (Exception^ e)
        {
            Log(L"condividi file: errore 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)) + L" " + path);
            ShowNotice(L"Impossibile condividere " + FileNameOf(path) + L".", nullptr);
        }
    }, task_continuation_context::use_current());
}

void MainPage::CopyToLibrary(const std::wstring& path, const wchar_t* folder, const wchar_t* label)
{
    std::wstring target = folder;
    std::wstring name = FileNameOf(path);
    std::wstring where = label;
    create_task([path, target, name]()
    {
        CreateDirectoryW(target.c_str(), nullptr);
        return SUCCEEDED(CopyFile2(path.c_str(), FreePath(target, name).c_str(), nullptr));
    }).then([this, name, where](bool copied)
    {
        ShowNotice(copied ? name + L" copiato in " + where + L"." : L"Copia di " + name + L" non riuscita.", nullptr);
    }, task_continuation_context::use_current());
}

// Out of the list; with deleteFile the file goes too. A download still running is cancelled first.
void MainPage::RemoveDownload(const DownloadEntry& entry, bool deleteFile)
{
    if (entry.state == 0 && entry.id && engine.downloadReply)
        engine.downloadReply(nullptr, entry.id, nullptr);
    if (deleteFile && !entry.path.empty())
        DeleteFileW(entry.path.c_str());
    m_downloads.erase(std::remove_if(m_downloads.begin(), m_downloads.end(), [&entry](const DownloadEntry& other)
    {
        return entry.id ? other.id == entry.id : other.path == entry.path;
    }), m_downloads.end());
    RefreshDownloads();
}

// ---- the page's file picker (<input type=file>) ----
// The WP8.1 pickers only show photos: a page that accepts only images (or videos) gets the system's photo picker, with
// its camera; anything else gets the list page as a small file manager of the phone's storage (and the memory card),
// with "foto" for the photo picker. Picked photos are copied into TemporaryFolder\upload, since the engine reads files
// by path; files from the list are passed as they are.

namespace
{
    // The page's accept list as extensions (".jpg"); none = any file. imagesOnly: the page takes nothing but images or
    // videos, which the photo picker shows.
    void ParseAccept(const std::wstring& accept, std::vector<std::wstring>& extensions, bool& imagesOnly)
    {
        static const struct { const wchar_t* type; const wchar_t* extensions; } known[] = {
            { L"image/*", L".jpg .jpeg .png .gif .bmp .webp .heic" },
            { L"video/*", L".mp4 .3gp .wmv .mov .avi .mkv .webm" },
            { L"audio/*", L".mp3 .m4a .aac .wma .wav .amr .ogg .opus .flac" },
            { L"image/jpeg", L".jpg .jpeg" }, { L"image/jpg", L".jpg .jpeg" }, { L"image/png", L".png" },
            { L"image/gif", L".gif" }, { L"image/webp", L".webp" }, { L"image/bmp", L".bmp" }, { L"image/heic", L".heic" },
            { L"video/mp4", L".mp4" }, { L"video/3gpp", L".3gp" }, { L"video/quicktime", L".mov" },
            { L"audio/mpeg", L".mp3" }, { L"audio/mp4", L".m4a" },
            { L"application/pdf", L".pdf" }, { L"text/plain", L".txt" }, { L"text/csv", L".csv" },
            { L"application/zip", L".zip" }, { L"application/msword", L".doc" },
            { L"application/vnd.openxmlformats-officedocument.wordprocessingml.document", L".docx" },
            { L"application/vnd.ms-excel", L".xls" },
            { L"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", L".xlsx" },
        };
        extensions.clear();
        imagesOnly = false;
        bool any = false;
        bool media = true;
        bool seen = false;
        for (auto token : Split(Lower(accept), L','))
        {
            token.erase(0, token.find_first_not_of(L" \t"));
            token.erase(token.find_last_not_of(L" \t") + 1);
            if (token.empty())
                continue;
            seen = true;
            if (token[0] == L'.')
            {
                extensions.push_back(token);
                media = media && (HasExtension(token, imageExtensions) || HasExtension(token, videoExtensions));
                continue;
            }
            media = media && (token.compare(0, 6, L"image/") == 0 || token.compare(0, 6, L"video/") == 0);
            bool found = false;
            for (auto& type : known)
            {
                if (token != type.type)
                    continue;
                for (auto& extension : Split(type.extensions, L' '))
                    extensions.push_back(extension);
                found = true;
                break;
            }
            // A type without a known extension: every file is shown.
            any = any || !found;
        }
        if (any)
            extensions.clear();
        imagesOnly = seen && media;
    }
}

void MainPage::PickFiles(lw_view* view, int id, bool multiple, const std::wstring& accept)
{
    // One at a time, and only for the tab on screen.
    if (m_pickId)
        AnswerPick(std::vector<std::wstring>());
    if (view != m_view)
    {
        if (engine.fileChooserReply)
            engine.fileChooserReply(view, id, nullptr, 0);
        return;
    }
    m_pickView = view;
    m_pickId = id;
    m_pickMultiple = multiple;
    m_pickSelected.clear();
    bool imagesOnly = false;
    ParseAccept(accept, m_pickExtensions, imagesOnly);
    Log(L"scelta file " + std::to_wstring(id) + (imagesOnly ? L": foto" : L": elenco") + (multiple ? L", piu' file" : L""));
    if (m_composeVisible)
        HideCompose(true, L"scelta file");
    if (imagesOnly)
        PickPhotos();
    else
        ShowFolder(m_pickFolder.empty() || !Exists(m_pickFolder) ? std::wstring(phoneFolder) : m_pickFolder);
}

void MainPage::PickPhotos()
{
    auto picker = ref new Windows::Storage::Pickers::FileOpenPicker();
    picker->ViewMode = Windows::Storage::Pickers::PickerViewMode::Thumbnail;
    picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::PicturesLibrary;
    // The page's own image and video types if it named some, else the usual ones of the phone's camera and screenshots.
    for (auto& extension : m_pickExtensions)
    {
        if (HasExtension(extension, imageExtensions) || HasExtension(extension, videoExtensions))
            picker->FileTypeFilter->Append(ref new String(extension.c_str()));
    }
    if (picker->FileTypeFilter->Size == 0)
    {
        for (auto extension : { L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".mp4" })
            picker->FileTypeFilter->Append(ref new String(extension));
    }
    try
    {
        // The app is suspended while the picker is up; App::OnActivated brings the photos to OnFilesPicked.
        if (m_pickMultiple)
            picker->PickMultipleFilesAndContinue();
        else
            picker->PickSingleFileAndContinue();
    }
    catch (Exception^ e)
    {
        Log(L"scelta foto: errore 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)));
        if (m_listKind != ListKind::Files)
            AnswerPick(std::vector<std::wstring>());
    }
}

void MainPage::OnFilesPicked(Windows::Foundation::Collections::IVectorView<StorageFile^>^ files)
{
    if (!m_pickId)
        return;
    int id = m_pickId;
    bool fromList = m_listKind == ListKind::Files;
    if (!files || files->Size == 0)
    {
        // Back from the photos: the list is still there to pick from; without it the page hears "cancel".
        if (!fromList)
            AnswerPick(std::vector<std::wstring>());
        return;
    }
    Log(L"scelta file " + std::to_wstring(id) + L": " + std::to_wstring(files->Size) + L" foto");
    std::vector<StorageFile^> picked;
    for (unsigned int i = 0; i < files->Size; ++i)
        picked.push_back(files->GetAt(i));
    create_task(ApplicationData::Current->TemporaryFolder->CreateFolderAsync(L"upload", CreationCollisionOption::OpenIfExists))
        .then([picked](StorageFolder^ folder)
    {
        std::vector<task<StorageFile^>> copies;
        for (auto file : picked)
        {
            copies.push_back(create_task(file->CopyAsync(folder, file->Name, NameCollisionOption::GenerateUniqueName))
                .then([](task<StorageFile^> copied) -> StorageFile^
            {
                try
                {
                    return copied.get();
                }
                catch (Exception^)
                {
                    return nullptr;
                }
            }));
        }
        return when_all(copies.begin(), copies.end());
    }).then([this, id](task<std::vector<StorageFile^>> copied)
    {
        if (m_pickId != id)
            return;
        std::vector<std::wstring> paths = m_pickSelected;
        try
        {
            for (auto file : copied.get())
            {
                if (file)
                    paths.push_back(file->Path->Data());
            }
        }
        catch (Exception^ e)
        {
            Log(L"scelta foto: copia non riuscita 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)));
        }
        AnswerPick(paths);
        if (m_listKind == ListKind::Files)
            HideList();
    }, task_continuation_context::use_current());
}

// One folder of the page's file list: subfolders, then the files the page accepts. A tap opens a folder or picks a
// file (with several allowed, it marks it, and "allega" sends the marked ones).
void MainPage::ShowFolder(const std::wstring& folder)
{
    m_pickFolder = folder;
    m_listKind = ListKind::Files;
    ListItems->Children->Clear();
    bool top = _wcsicmp(folder.c_str(), phoneFolder) == 0;
    std::wstring name = top ? std::wstring(L"telefono") : folder.size() <= 3 ? std::wstring(L"scheda SD") : FileNameOf(folder);
    ListTitle1->Text = ref new String(Lower(name).c_str());
    ListTitle1->Foreground = ref new SolidColorBrush(Windows::UI::Colors::White);
    ListTitle2->Text = m_pickMultiple ? L"foto" : L"";
    ListTitle2->Foreground = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0x77, 0x77, 0x77));

    struct Item { std::wstring name; bool folder; long long size; };
    std::vector<Item> folders, files;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileExW((folder + (folder.back() == L'\\' ? L"*" : L"\\*")).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring entry = data.cFileName;
            if (entry == L"." || entry == L".." || (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                continue;
            bool isFolder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!isFolder && !m_pickExtensions.empty()
                && std::find(m_pickExtensions.begin(), m_pickExtensions.end(), ExtensionOf(entry)) == m_pickExtensions.end())
                continue;
            Item item;
            item.name = entry;
            item.folder = isFolder;
            item.size = (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            (isFolder ? folders : files).push_back(item);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    auto byName = [](const Item& a, const Item& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    std::wstring base = folder.back() == L'\\' ? folder : folder + L"\\";
    if (!top)
        AddListRow(L"..", L"cartella superiore", [this, folder]() { ShowFolder(ParentFolder(folder)); }, nullptr, nullptr);
    // The memory card, when there is one, from the top of the phone's storage.
    if (top && Exists(L"D:\\"))
        AddListRow(L"scheda SD", L"D:\\", [this]() { ShowFolder(L"D:\\"); }, nullptr, nullptr);
    for (auto& item : folders)
    {
        std::wstring path = base + item.name;
        AddListRow(item.name, L"cartella", [this, path]() { ShowFolder(path); }, nullptr, nullptr);
    }
    for (auto& item : files)
    {
        std::wstring path = base + item.name;
        bool marked = std::find(m_pickSelected.begin(), m_pickSelected.end(), path) != m_pickSelected.end();
        AddListRow(item.name, (marked ? L"\u2713 scelto, " : L"") + SizeText(item.size), [this, path]()
        {
            if (!m_pickMultiple)
            {
                AnswerPick(std::vector<std::wstring>(1, path));
                HideList();
                return;
            }
            auto it = std::find(m_pickSelected.begin(), m_pickSelected.end(), path);
            if (it == m_pickSelected.end())
                m_pickSelected.push_back(path);
            else
                m_pickSelected.erase(it);
            double offset = ListScroll->VerticalOffset;
            ShowFolder(m_pickFolder);
            ListScroll->ChangeView(nullptr, ref new Box<double>(offset), nullptr, true);
        }, nullptr, nullptr);
    }
    if (m_pickMultiple)
        ListActionText->Text = ref new String((L"allega (" + std::to_wstring(m_pickSelected.size()) + L")").c_str());
    else
        ListActionText->Text = L"foto";
    ListEmpty->Text = m_pickExtensions.empty() ? L"Cartella vuota." : L"Nessun file del tipo che la pagina accetta in questa cartella.";
    ListEmpty->Visibility = folders.empty() && files.empty() ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    ListScroll->ChangeView(nullptr, ref new Box<double>(0.0), nullptr, true);
    ListPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

// The page's <input> gets these files (none = cancelled); the pick is over.
void MainPage::AnswerPick(const std::vector<std::wstring>& paths)
{
    lw_view* view = m_pickView;
    int id = m_pickId;
    m_pickView = nullptr;
    m_pickId = 0;
    m_pickSelected.clear();
    if (!id || !engine.fileChooserReply)
        return;
    std::vector<const wchar_t*> pointers;
    for (auto& path : paths)
        pointers.push_back(path.c_str());
    Log(L"scelta file " + std::to_wstring(id) + L": " + (paths.empty() ? L"annullata" : std::to_wstring(paths.size()) + L" file"));
    engine.fileChooserReply(view, id, pointers.empty() ? nullptr : pointers.data(), static_cast<int>(pointers.size()));
}

// ---- notice ----

void MainPage::ShowNotice(const std::wstring& text, std::function<void()> action)
{
    NoticeText->Text = ref new String(text.c_str());
    m_noticeAction = action;
    Notice->Visibility = Windows::UI::Xaml::Visibility::Visible;
    if (!m_noticeTimer)
    {
        m_noticeTimer = ref new DispatcherTimer();
        TimeSpan interval;
        interval.Duration = 5 * 10000000LL;
        m_noticeTimer->Interval = interval;
        m_noticeTimer->Tick += ref new EventHandler<Object^>([this](Object^, Object^)
        {
            m_noticeTimer->Stop();
            Notice->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            m_noticeAction = nullptr;
        });
    }
    m_noticeTimer->Stop();
    m_noticeTimer->Start();
}

void MainPage::Notice_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    Notice->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (m_noticeTimer)
        m_noticeTimer->Stop();
    auto action = m_noticeAction;
    m_noticeAction = nullptr;
    if (action)
        action();
}

// ---- site apps (Site.h, PLAN §64.1) ----
// The same shell with the browser taken away: the bar keeps the page's title, reload and the menu. The PIN screen comes
// first; the site's username and password live in the vault (Vault.h), decrypted into memory only after the PIN, and
// go into the page's login form, on the app's own hosts over https only.

namespace
{
    const int fillScriptId = 0x5E1;

    // The visible password field, and the closest visible text field before it in the same form: each gets the value
    // through the input's own setter (frameworks that track the value see it) and the input and change events, only
    // if it is empty. The answer says how many were filled, never what.
    const wchar_t* const fillScript =
        L"(function(u,p){"
        L"function put(e,v){if(!e||e.value)return 0;"
        L"Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(e,v);"
        L"e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));return 1;}"
        L"function shown(e){return !e.disabled&&!e.readOnly&&(e.offsetWidth||e.offsetHeight||e.getClientRects().length);}"
        L"var pw=[].filter.call(document.querySelectorAll('input[type=password]'),shown);"
        L"if(!pw.length){var id=[].filter.call(document.querySelectorAll('input[type=email],input[autocomplete~=username]'),shown);"
        L"if(id.length!==1)return 'accesso: nessun campo';"
        L"return 'accesso: compilati '+put(id[0],u);}"
        L"var f=pw[0],scope=f.form||document,user=null;"
        L"var c=scope.querySelectorAll('input[type=text],input[type=email],input[type=tel],input:not([type])');"
        L"for(var i=0;i<c.length;i++){if(shown(c[i])&&(c[i].compareDocumentPosition(f)&4))user=c[i];}"
        L"return 'accesso: compilati '+(put(user,u)+put(f,p));"
        L"})(";

    std::wstring JsString(const std::wstring& text)
    {
        std::wstring out = L"\"";
        for (wchar_t c : text)
        {
            if (c == L'\\')
                out += L"\\\\";
            else if (c == L'"')
                out += L"\\\"";
            else if (c < 0x20 || c == 0x2028 || c == 0x2029)
            {
                wchar_t escaped[8];
                swprintf_s(escaped, L"\\u%04x", static_cast<unsigned>(c));
                out += escaped;
            }
            else
                out += c;
        }
        return out + L"\"";
    }
}

namespace
{
    Windows::UI::Xaml::Media::SolidColorBrush^ SolidBrush(unsigned int argb)
    {
        return ref new Windows::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(
            static_cast<unsigned char>(argb >> 24), static_cast<unsigned char>(argb >> 16),
            static_cast<unsigned char>(argb >> 8), static_cast<unsigned char>(argb)));
    }
    const unsigned int keyColor = 0xFF262626;
    const unsigned int keyPressedColor = 0xFF404040;
    const size_t pinMax = 12;
}

void MainPage::ApplySiteMode()
{
    Favorites->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    Address->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    TabsButton->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    SiteTitle->Text = ref new String(Site::name);
    SiteTitle->Visibility = Windows::UI::Xaml::Visibility::Visible;
    LockTitle->Text = ref new String(Site::name);
    LockBand->Fill = SolidBrush(Site::color);
    LockNext->Background = SolidBrush(Site::color);
    LockNoPin->Background = SolidBrush(0xFF333333);
    BuildKeypad();
    if (NoPin() && Vault::Exists())
    {
        // No PIN screen: the vault opens with the fixed secret; the PIN screen only if that fails.
        m_lockBusy = true;
        Vault::Open(noPinSecret).then([this](std::shared_ptr<Vault::Credentials> credentials)
        {
            m_lockBusy = false;
            if (!credentials)
            {
                Log(L"vault: apertura senza PIN non riuscita");
                ShowLock(nullptr);
                return;
            }
            m_siteUser = credentials->user;
            m_sitePassword = credentials->password;
            Vault::Wipe(credentials->user);
            Vault::Wipe(credentials->password);
            m_unlocked = true;
            Log(L"vault: aperto senza PIN");
            if (auto tab = ActiveTab())
                FillLogin(tab->view, tab->url);
        }, task_continuation_context::use_current());
        return;
    }
    ShowLock(nullptr);
}

// 1-9, then delete, 0, OK: square keys that light up under the finger at once (PointerPressed, not Tapped).
void MainPage::BuildKeypad()
{
    for (int i = 0; i < 3; ++i)
        Keypad->ColumnDefinitions->Append(ref new Windows::UI::Xaml::Controls::ColumnDefinition());
    for (int i = 0; i < 4; ++i)
    {
        auto row = ref new Windows::UI::Xaml::Controls::RowDefinition();
        row->Height = Windows::UI::Xaml::GridLengthHelper::FromPixels(76);
        Keypad->RowDefinitions->Append(row);
    }
    const wchar_t keys[] = L"123456789\b0\n";
    for (int i = 0; i < 12; ++i)
    {
        wchar_t key = keys[i];
        bool ok = key == L'\n';
        unsigned int rest = ok ? Site::color : keyColor;
        auto cell = ref new Border();
        cell->Margin = Windows::UI::Xaml::ThicknessHelper::FromUniformLength(4);
        cell->Background = SolidBrush(rest);
        auto label = ref new TextBlock();
        label->Text = key == L'\b' ? L"\u232B" : ok ? L"\u2713" : ref new String(&key, 1);
        label->FontFamily = ref new Windows::UI::Xaml::Media::FontFamily(key == L'\b' || ok ? L"Segoe UI Symbol" : L"Segoe WP");
        label->FontSize = 34;
        label->FontWeight = Windows::UI::Text::FontWeights::Light;
        label->Foreground = SolidBrush(0xFFFFFFFF);
        label->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Center;
        label->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        cell->Child = label;
        Grid::SetRow(cell, i / 3);
        Grid::SetColumn(cell, i % 3);
        cell->PointerPressed += ref new PointerEventHandler([this, cell, key](Object^, PointerRoutedEventArgs^ e)
        {
            e->Handled = true;
            cell->Background = SolidBrush(key == L'\n' ? (Site::color & 0x00FFFFFF) | 0xB0000000 : keyPressedColor);
            LockKey(key);
        });
        auto release = ref new PointerEventHandler([cell, rest](Object^, PointerRoutedEventArgs^) { cell->Background = SolidBrush(rest); });
        cell->PointerReleased += release;
        cell->PointerExited += release;
        cell->PointerCaptureLost += release;
        Keypad->Children->Append(cell);
    }
}

// The PIN screen, also to lock again (menu, or a long time in the background): the credentials leave memory.
void MainPage::ShowLock(const wchar_t* message)
{
    Vault::Wipe(m_siteUser);
    Vault::Wipe(m_sitePassword);
    m_unlocked = false;
    m_lockSetup = !Vault::Exists();
    HideCompose(true, L"schermata del PIN");
    LockUser->Text = L"";
    LockPassword->Password = L"";
    Lock->Visibility = Windows::UI::Xaml::Visibility::Visible;
    ShowLockStep(m_lockSetup ? 0 : 3, message);
}

void MainPage::ShowLockStep(int step, const wchar_t* message)
{
    m_lockStep = step;
    Vault::Wipe(m_pinEntry);
    if (step <= 1)
        Vault::Wipe(m_pinFirst);
    bool fields = step == 0;
    LockSetup->Visibility = fields ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    LockDots->Visibility = fields ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    Keypad->Visibility = LockDots->Visibility;
    const wchar_t* text = message;
    if (!text)
        text = step == 0 ? L"Scrivi una volta nome utente e password del sito: restano sul telefono, cifrati dal PIN."
             : step == 1 ? L"Scegli un PIN (da 4 a 12 cifre)."
             : step == 2 ? L"Ripeti il PIN."
             : L"PIN";
    LockMessage->Text = ref new String(text);
    LockOther->Text = step == 0 ? L"entra senza accesso automatico" : step == 3 ? L"dimentica le credenziali" : L"indietro";
    UpdatePinDots();
    if (fields)
        LockUser->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    else
        HideKeyboard();
}

void MainPage::UpdatePinDots()
{
    LockDots->Children->Clear();
    for (size_t i = 0; i < m_pinEntry.size(); ++i)
    {
        auto dot = ref new Windows::UI::Xaml::Shapes::Rectangle();
        dot->Width = 16;
        dot->Height = 16;
        dot->Margin = Windows::UI::Xaml::ThicknessHelper::FromLengths(6, 0, 6, 0);
        dot->Fill = SolidBrush(0xFFFFFFFF);
        LockDots->Children->Append(dot);
    }
}

void MainPage::HideLock()
{
    LockUser->Text = L"";
    LockPassword->Password = L"";
    Vault::Wipe(m_pinEntry);
    Vault::Wipe(m_pinFirst);
    UpdatePinDots();
    Lock->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    HideKeyboard();
}

void MainPage::LockNoPin_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    if (!LockUser->Text->Length() || !LockPassword->Password->Length())
    {
        LockMessage->Text = L"Servono nome utente e password del sito.";
        return;
    }
    SetupVault(noPinSecret);
}

void MainPage::LockNext_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    if (!LockUser->Text->Length() || !LockPassword->Password->Length())
    {
        LockMessage->Text = L"Servono nome utente e password del sito.";
        return;
    }
    ShowLockStep(1, nullptr);
}

void MainPage::LockKey(wchar_t key)
{
    if (m_lockBusy || m_lockStep == 0)
        return;
    if (key == L'\b')
    {
        if (!m_pinEntry.empty())
            m_pinEntry.pop_back();
        UpdatePinDots();
        return;
    }
    if (key != L'\n')
    {
        if (m_pinEntry.size() < pinMax)
            m_pinEntry += key;
        UpdatePinDots();
        return;
    }
    if (m_pinEntry.size() < 4)
    {
        LockMessage->Text = L"Il PIN ha almeno 4 cifre.";
        return;
    }
    if (m_lockStep == 1)
    {
        std::wstring first = m_pinEntry;
        ShowLockStep(2, nullptr);
        m_pinFirst = first;
        Vault::Wipe(first);
        return;
    }
    if (m_lockStep == 2)
    {
        if (m_pinEntry != m_pinFirst)
        {
            ShowLockStep(1, L"I due PIN non coincidono: scegline uno di nuovo.");
            return;
        }
        SetupVault(m_pinEntry);
        return;
    }
    UnlockVault();
}

void MainPage::LockOther_Tapped(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    if (m_lockBusy)
        return;
    if (m_lockStep == 0)
    {
        // The site without autofill; the PIN screen comes back at the next start.
        HideLock();
        return;
    }
    if (m_lockStep == 1 || m_lockStep == 2)
    {
        if (m_changingPin)
        {
            m_changingPin = false;
            HideLock();
            return;
        }
        ShowLockStep(0, nullptr);
        return;
    }
    ForgetCredentials();
}

void MainPage::ForgetCredentials()
{
    ShowAppDialog(Site::name, L"Cancello nome utente e password salvati? Li riscrivi con un PIN nuovo.", L"", L"cancella",
        [this](bool accepted, const std::wstring&)
    {
        if (!accepted)
            return;
        Vault::Remove();
        ApplicationData::Current->LocalSettings->Values->Insert(L"noPin", PropertyValue::CreateBoolean(false));
        m_unlocked = false;
        Log(L"vault: cancellato");
        ShowLock(nullptr);
    });
}

// A new vault: from the fields of the first screen, or (PIN changed, PIN added or taken away) from the credentials
// already open. pin == noPinSecret: the app opens without the PIN screen from now on.
void MainPage::SetupVault(const std::wstring& newPin)
{
    if (m_lockBusy)
        return;
    bool fromFields = !m_unlocked;
    std::wstring user = fromFields ? std::wstring(LockUser->Text->Data()) : m_siteUser;
    std::wstring password = fromFields ? std::wstring(LockPassword->Password->Data()) : m_sitePassword;
    std::wstring pin = newPin;
    bool noPin = newPin == noPinSecret;
    m_lockBusy = true;
    LockMessage->Text = L"Cifro...";
    Vault::Credentials credentials;
    credentials.user = user;
    credentials.password = password;
    ULONGLONG started = GetTickCount64();
    Vault::Create(pin, credentials).then([this, user, password, started, noPin](task<void> created)
    {
        m_lockBusy = false;
        try
        {
            created.get();
        }
        catch (Exception^ e)
        {
            Log(L"vault: creazione non riuscita 0x" + std::to_wstring(static_cast<unsigned>(e->HResult)));
            if (Lock->Visibility == Windows::UI::Xaml::Visibility::Visible)
                ShowLockStep(1, L"Non riesco a salvare le credenziali. Riprova.");
            return;
        }
        Log(L"vault: creato in " + std::to_wstring(GetTickCount64() - started) + L" ms" + (noPin ? L", senza PIN" : L""));
        ApplicationData::Current->LocalSettings->Values->Insert(L"noPin", PropertyValue::CreateBoolean(noPin));
        m_changingPin = false;
        m_siteUser = user;
        m_sitePassword = password;
        m_unlocked = true;
        HideLock();
        if (auto tab = ActiveTab())
            FillLogin(tab->view, tab->url);
    }, task_continuation_context::use_current());
    Vault::Wipe(pin);
    Vault::Wipe(password);
    Vault::Wipe(credentials.password);
}

void MainPage::UnlockVault()
{
    if (m_lockBusy)
        return;
    ULONGLONG now = GetTickCount64();
    if (now < m_lockWaitUntil)
    {
        LockMessage->Text = ref new String((L"Troppi tentativi: riprova tra " + std::to_wstring((m_lockWaitUntil - now) / 1000 + 1) + L" s.").c_str());
        return;
    }
    std::wstring pin = m_pinEntry;
    m_lockBusy = true;
    LockMessage->Text = L"Controllo...";
    ULONGLONG started = now;
    Vault::Open(pin).then([this, started](std::shared_ptr<Vault::Credentials> credentials)
    {
        m_lockBusy = false;
        if (!credentials)
        {
            // Past five wrong PINs each try waits longer (30 s, 1 min, 2 min... at most 30 min).
            ++m_lockFailures;
            Log(L"vault: PIN sbagliato (" + std::to_wstring(m_lockFailures) + L")");
            if (m_lockFailures >= 5)
                m_lockWaitUntil = GetTickCount64() + std::min(30000ULL << std::min(m_lockFailures - 5, 6), 1800000ULL);
            ShowLockStep(3, L"PIN sbagliato.");
            return;
        }
        Log(L"vault: aperto in " + std::to_wstring(GetTickCount64() - started) + L" ms");
        m_lockFailures = 0;
        m_siteUser = credentials->user;
        m_sitePassword = credentials->password;
        Vault::Wipe(credentials->user);
        Vault::Wipe(credentials->password);
        m_unlocked = true;
        HideLock();
        if (auto tab = ActiveTab())
            FillLogin(tab->view, tab->url);
    }, task_continuation_context::use_current());
    Vault::Wipe(pin);
}

// Back after more than 10 minutes away: the PIN again.
void MainPage::OnSiteResumed(unsigned long long awayMs)
{
    if (!Site::app || !m_unlocked || NoPin() || awayMs < 10 * 60 * 1000ULL)
        return;
    Log(L"vault: bloccato dopo " + std::to_wstring(awayMs / 60000) + L" min in background");
    ShowLock(L"Bloccata dopo un po' fuori dall'app.");
}

bool MainPage::IsSiteURL(const std::wstring& url)
{
    std::wstring host = HostOf(url);
    if (host.empty())
        return false;
    std::wstring list = Site::hosts;
    for (size_t start = 0; start < list.size();)
    {
        size_t end = std::min(list.find(L' ', start), list.size());
        std::wstring site = list.substr(start, end - start);
        start = end + 1;
        if (site.empty())
            continue;
        if (host == site || (host.size() > site.size() + 1 && host.compare(host.size() - site.size() - 1, site.size() + 1, L"." + site) == 0))
            return true;
    }
    return false;
}

void MainPage::FillLogin(lw_view* view, const std::wstring& url)
{
    if (!Site::app || !m_unlocked || !view || !engine.runScript || m_sitePassword.empty())
        return;
    if (url.compare(0, 8, L"https://") != 0 || !IsSiteURL(url))
        return;
    std::wstring script = std::wstring(fillScript) + JsString(m_siteUser) + L"," + JsString(m_sitePassword) + L")";
    engine.runScript(view, fillScriptId, script.c_str());
    Vault::Wipe(script);
}
