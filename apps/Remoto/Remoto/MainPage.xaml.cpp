// Remoto: the Pixel 9a's apps on the Lumia (PLAN §64, §65). The Pixel (ponte/agent Remoto.java + RemotoStream.java,
// port 8795) runs them on a virtual display of this phone's size and sends its H.264 stream, paced on our
// acknowledgements ('F' for every frame, at once): it never sends more than the path carries, so here every access
// unit is decoded on the GPU as it arrives (Core/Video), on the network thread, with no queue and nothing skipped;
// only a frame with a newer one already received is not presented. Touches go back from the panel's own input
// thread, the Back key as Android's Back, the Lumia keyboard as text. tools/remotonet plays this client on the PC
// (through our Tailscale code, with a slow mobile link emulated); tools/remotobench plays the Pixel.
#include "pch.h"
#include "MainPage.xaml.h"
#include "Core/Video.h"
#include "Core/Audio.h"
#include "Core/Mic.h"
#include "tailnet.h"
#include <winsock2.h>
#include <atomic>
#include <climits>
#include <mutex>
#include <thread>

using namespace Remoto;
using namespace concurrency;
using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Foundation;
using namespace Windows::Phone::UI::Input;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    const wchar_t* PixelServer = L"192.168.1.112:8795"; // the 9a (ponte), static on the LAN
    const wchar_t* BenchServer = L"192.168.1.148:8795"; // the PC, tools/remotobench
    const wchar_t* PairUrl = L"http://192.168.1.148:8765/ponte-pair.txt"; // "<host> <key>"
    const char* TailnetPixel = "ponte-pixel";                           // the 9a's Tailscale node

    void WinRandom(uint8_t* out, size_t n)
    {
        Array<unsigned char>^ a;
        CryptographicBuffer::CopyToByteArray(CryptographicBuffer::GenerateRandom(static_cast<unsigned int>(n)), &a);
        memcpy(out, a->Data, n);
    }

    // Lines for the PC (tools/inbox: share/inbox/remoto.txt): the Tailscale login link, failures.
    void Inbox(const std::string& line)
    {
        try
        {
            auto http = ref new Windows::Web::Http::HttpClient();
            auto content = ref new Windows::Web::Http::HttpStringContent(ref new String(std::wstring(line.begin(), line.end()).c_str()));
            create_task(http->PostAsync(ref new Uri(L"http://192.168.1.148:8765/remoto"), content)).then([http](task<Windows::Web::Http::HttpResponseMessage^> t)
            {
                try { t.get(); } catch (Exception^) { }
            });
        }
        catch (Exception^) { }
    }

    // The app's log: LocalState\run.log (tools/phonelog b0f25e4a-d002-447f-af07-46ce69f16306 reads it over USB,
    // also when the phone was on 4G and the PC was out of reach) and the PC inbox when reachable.
    std::mutex g_logLock;
    void Log(const std::string& line)
    {
        static std::wstring path = std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\run.log";
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        sprintf_s(stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        {
            std::lock_guard<std::mutex> lock(g_logLock);
            static bool started = false;
            FILE* f = nullptr;
            _wfopen_s(&f, path.c_str(), started ? L"ab" : L"wb"); // a new file at each start
            started = true;
            if (f)
            {
                fputs((stamp + line + "\n").c_str(), f);
                fclose(f);
            }
        }
        Inbox(line);
    }

    // The Tailscale node of this app ("lumia-remoto"), started once; outside home the Pixel is reached through it.
    std::shared_ptr<ts::Tailnet> g_tailnet;
    std::mutex g_tailnetLock;
    std::string g_tailnetLogin;

    long long NowUs()
    {
        static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart * 1000000 / frequency.QuadPart;
    }

    std::wstring Wide(const std::string& text)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(n > 0 ? n : 0, L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n);
        return out;
    }

    std::string Utf8(const std::wstring& text)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n : 0, '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
        return out;
    }

    // "avg/p90" in ms of a list of microseconds.
    std::string Summary(std::vector<long long> values)
    {
        if (values.empty())
            return "-";
        std::sort(values.begin(), values.end());
        long long sum = 0;
        for (auto v : values)
            sum += v;
        char text[48];
        sprintf_s(text, "%.1f/%.1f", sum / 1000.0 / values.size(), values[values.size() * 9 / 10] / 1000.0);
        return text;
    }

    // "a.b.c.d" in network order (inet_pton is not in the phone SDK).
    unsigned long ParseIPv4(const std::string& text)
    {
        unsigned int parts[4] = {};
        sscanf_s(text.c_str(), "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]);
        return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    }

    template <typename T> T Read(const unsigned char* p) { T v; memcpy(&v, p, sizeof v); return v; }

    Windows::Foundation::Collections::IPropertySet^ Settings()
    {
        return Windows::Storage::ApplicationData::Current->LocalSettings->Values;
    }

    std::wstring Setting(const wchar_t* name, const wchar_t* fallback)
    {
        auto values = Settings();
        if (values->HasKey(StringReference(name)))
            if (auto s = dynamic_cast<String^>(values->Lookup(StringReference(name))))
                return s->Data();
        return fallback;
    }

    // HMAC-SHA256 (WinRT; any thread).
    std::vector<unsigned char> Hmac(const std::vector<unsigned char>& key, const unsigned char* data, size_t size)
    {
        auto provider = MacAlgorithmProvider::OpenAlgorithm(MacAlgorithmNames::HmacSha256);
        auto k = provider->CreateKey(CryptographicBuffer::CreateFromByteArray(
            ArrayReference<unsigned char>(const_cast<unsigned char*>(key.data()), static_cast<unsigned int>(key.size()))));
        auto mac = CryptographicEngine::Sign(k, CryptographicBuffer::CreateFromByteArray(
            ArrayReference<unsigned char>(const_cast<unsigned char*>(data), static_cast<unsigned int>(size))));
        Array<unsigned char>^ out = nullptr;
        CryptographicBuffer::CopyToByteArray(mac, &out);
        return std::vector<unsigned char>(out->begin(), out->end());
    }

    // The ponte key (64 hex characters) -> its MAC key, HMAC(secret, "mac") as ponte/agent Server.java.
    std::vector<unsigned char> MacKey(const std::wstring& hex)
    {
        std::vector<unsigned char> secret;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            secret.push_back(static_cast<unsigned char>(wcstoul(hex.substr(i, 2).c_str(), nullptr, 16)));
        if (secret.size() != 32)
            return std::vector<unsigned char>();
        const char* label = "mac";
        return Hmac(secret, reinterpret_cast<const unsigned char*>(label), 3);
    }
}

namespace Remoto
{
    // The connection to the Pixel: a LAN socket or a TCP connection inside Tailscale, read through our own buffer
    // (so "is more queued?" works for both: the catch-up's head-of-stream test).
    struct Conn
    {
        SOCKET socket = INVALID_SOCKET;
        std::shared_ptr<ts::TcpConn> tcp;
        std::vector<unsigned char> buffer;
        size_t offset = 0;
        std::atomic<bool> closed;
        int lastRead = 0;
        int lastError = 0;
        Conn() : closed(false) { }

        bool Fill()
        {
            if (offset == buffer.size())
            {
                buffer.clear();
                offset = 0;
            }
            size_t at = buffer.size();
            buffer.resize(at + 65536);
            int n;
            if (tcp)
                while ((n = tcp->read(buffer.data() + at, 65536, 1000)) < 0 && !closed) { }
            else
            {
                n = recv(socket, reinterpret_cast<char*>(buffer.data() + at), 65536, 0);
                if (n <= 0)
                    lastError = WSAGetLastError();
            }
            buffer.resize(at + (n > 0 ? n : 0));
            lastRead = n;
            return n > 0;
        }

        // Why the stream ended: the peer closed, the stack died (Tailscale retransmission timeout, RST), or we
        // closed it ourselves. Without this the log only said "collegamento chiuso".
        std::string Why()
        {
            if (closed)
                return "chiusa da noi";
            if (tcp)
            {
                std::string e = tcp->error();
                return e.empty() ? "chiusa dal ponte" : e;
            }
            if (lastRead == 0)
                return "chiusa dal ponte (recv 0)";
            return "errore di rete " + std::to_string(lastError);
        }

        bool ReceiveAll(void* data, size_t size)
        {
            unsigned char* p = static_cast<unsigned char*>(data);
            while (size)
            {
                if (offset == buffer.size() && !Fill())
                    return false;
                size_t k = std::min(size, buffer.size() - offset);
                memcpy(p, buffer.data() + offset, k);
                offset += k;
                p += k;
                size -= k;
            }
            return true;
        }

        // A whole video message ('V' + 16-byte header + body) is already in our buffer: the one being decoded is
        // not the newest, presenting it would only cost a vsync.
        bool HasCompleteVideo()
        {
            size_t n = buffer.size() - offset;
            if (n < 17 || buffer[offset] != 'V')
                return false;
            unsigned int size;
            memcpy(&size, buffer.data() + offset + 1, 4);
            return n >= 17 + static_cast<size_t>(size);
        }

        size_t Queued()
        {
            size_t n = buffer.size() - offset;
            if (tcp)
                n += tcp->queued();   // the tstcp stack may hold more than our read buffer: still "head of stream"
            else
            {
                u_long more = 0;
                ioctlsocket(socket, FIONREAD, &more);
                n += more;
            }
            return n;
        }

        bool Send(const void* data, size_t size)
        {
            if (tcp)
                return tcp->write(static_cast<const uint8_t*>(data), size);
            return ::send(socket, static_cast<const char*>(data), static_cast<int>(size), 0) == static_cast<int>(size);
        }

        void Close()
        {
            if (closed.exchange(true))
                return;
            if (tcp)
                tcp->close();
            else if (socket != INVALID_SOCKET)
                closesocket(socket);
        }
    };

    struct Link
    {
        Video video;
        Audio audio;
        bool audioTried = false, audioReady = false;
        std::atomic<bool> stop;
        std::shared_ptr<Conn> conn;   // under sendLock
        std::mutex sendLock;
        std::thread thread;
        std::thread pinger;
        std::string server;
        int port = 8795;
        int width = 768, height = 1280; // the stream (the Pixel's virtual display = the Lumia's screen)
        std::vector<unsigned char> macKey;
        bool tailnet = false;
        int tsAttempts = 0;
        int blind = 0;             // frames in a row the decoder gave no picture for
        long long keyAsked = 0;
        bool tsOnly = false;  // menu "solo Tailscale": the LAN is skipped (tests at home) // the Pixel (not the bench): Tailscale when the LAN does not answer

        // clock of the other side: server_us = phone_us + offset, from the ping with the shortest round trip
        std::atomic<long long> offset;
        std::atomic<bool> offsetKnown;
        long long bestRtt = LLONG_MAX;

        // stats of the last 2 s
        std::vector<long long> net, decode, convert, present, rtt;
        size_t bytes = 0;
        int frames = 0;
        long long statsAt = 0;
        long long statsSentAt = 0;
        bool decoderTried = false;
        bool decoderReady = false;
        int resets = 0;            // key frames asked because the decoder lost the picture
        int unshown = 0;           // frames decoded but not presented (a newer one was already here)

        Link() : stop(false), offset(0), offsetKnown(false) { }

        std::shared_ptr<Conn> Current()
        {
            std::lock_guard<std::mutex> lock(sendLock);
            return conn;
        }

        void Send(char kind, const void* data, size_t size)
        {
            std::lock_guard<std::mutex> lock(sendLock);
            if (!conn)
                return;
            std::string message(1, kind);
            if (size)
                message.append(static_cast<const char*>(data), size);
            conn->Send(message.data(), message.size());
        }

        void SendBlock(char kind, const std::string& text)
        {
            std::string body(4, '\0');
            unsigned int n = static_cast<unsigned int>(text.size());
            memcpy(&body[0], &n, 4);
            body += text;
            Send(kind, body.data(), body.size());
        }

        void SendText(const std::string& text) { SendBlock('S', text); }

        void Key(int code)
        {
            unsigned short c = static_cast<unsigned short>(code);
            Send('B', &c, 2);
        }
    };
}

MainPage::MainPage() : m_link(nullptr), m_showTimings(false), m_typing(false), m_appsAsked(false), m_listOpen(false), m_menuOpen(false), m_openOpen(false), m_rotation(0), m_pinOpen(false), m_pinTries(0), m_lastVideo(0),
                       m_panelWidth(0), m_panelHeight(0), m_fieldSeq(0), m_fieldOpen(false), m_copySeq(0), m_micWanted(false)
{
    InitializeComponent();
    for (int i = 0; i < 10; ++i)
    {
        m_pointerIds[i] = -1;
        m_edge[i] = false;
        m_startY[i] = 0;
    }
    m_appLabels = ref new Vector<String^>();
    AppList->ItemsSource = m_appLabels;
    UpdateServerButton();

    Search->TextChanged += ref new TextChangedEventHandler([this](Object^, TextChangedEventArgs^) { Filter(); });

    Screen->SizeChanged += ref new SizeChangedEventHandler([this](Object^, SizeChangedEventArgs^ e)
    {
        m_panelWidth = e->NewSize.Width;
        m_panelHeight = e->NewSize.Height;
    });

    // Lumia keyboard -> Pixel: each change of the invisible box is sent as text and the box emptied.
    Typing->TextChanged += ref new TextChangedEventHandler([this](Object^, TextChangedEventArgs^)
    {
        std::wstring text = Typing->Text->Data();
        if (text.empty())
            return;
        Typing->Text = L"";
        if (m_link)
            m_link->SendBlock('X', Utf8(text));
    });
    Typing->KeyDown += ref new Windows::UI::Xaml::Input::KeyEventHandler([this](Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
    {
        if (!m_link)
            return;
        if (e->Key == Windows::System::VirtualKey::Back)
        {
            m_link->Key(67); // KEYCODE_DEL
            e->Handled = true;
        }
        else if (e->Key == Windows::System::VirtualKey::Enter)
        {
            m_link->Key(66); // KEYCODE_ENTER
            e->Handled = true;
        }
    });
    Typing->GotFocus += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { m_typing = true; });
    // Disabled except while typing: enabled, it took the focus by itself (list closed, app started) and the
    // keyboard came up.
    Typing->LostFocus += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        m_typing = false;
        Typing->IsEnabled = false;
    });
}

void MainPage::OnNavigatedTo(NavigationEventArgs^)
{
    // Full screen: the Pixel's picture is exactly this screen; the minimal app bar lies over its bottom edge.
    Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(
        Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
    create_task(Windows::UI::ViewManagement::StatusBar::GetForCurrentView()->HideAsync()).then([](task<void> t)
    {
        try { t.get(); } catch (Exception^) { }
    });
    // The screen stays on while the Pixel is shown.
    static auto request = ref new Windows::System::Display::DisplayRequest();
    request->RequestActive();
    HardwareButtons::BackPressed += ref new EventHandler<BackPressedEventArgs^>(this, &MainPage::OnBack);
    // The page turns as Windows says (rotation lock included), the Pixel's picture does not: it is always the
    // 768x1280 stream on the phone's pixels. How the phone is held goes to the Pixel ('W'), whose display turns the
    // picture inside the stream, as a phone's panel does (Orient).
    Windows::Graphics::Display::DisplayInformation::AutoRotationPreferences =
        Windows::Graphics::Display::DisplayOrientations::Portrait | Windows::Graphics::Display::DisplayOrientations::Landscape
        | Windows::Graphics::Display::DisplayOrientations::LandscapeFlipped;
    Orient();
    Window::Current->SizeChanged += ref new WindowSizeChangedEventHandler([this](Object^, Windows::UI::Core::WindowSizeChangedEventArgs^) { Orient(); });
    // The Lumia keyboard's height goes to the Pixel ('h'): its keyboard (RemotoIme) is an empty strip that tall, so
    // the app there shrinks above it and the field stays in view. Our page never pans: the picture stays put.
    auto pane = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
    pane->Showing += ref new TypedEventHandler<Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
        [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e)
    {
        e->EnsuredFocusedElementInView = true;
        float scale = static_cast<float>(Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->RawPixelsPerViewPixel);
        unsigned short px = static_cast<unsigned short>(e->OccludedRect.Height * scale + 0.5f);
        if (m_link && !m_listOpen && !m_openOpen)
            m_link->Send('h', &px, 2);
    });
    pane->Hiding += ref new TypedEventHandler<Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
        [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e)
    {
        e->EnsuredFocusedElementInView = true;
        unsigned short zero = 0;
        m_fieldOpen = false;
        if (m_link)
            m_link->Send('h', &zero, 2);
    });
    Screen->Loaded += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        StartInput();
        if (Setting(L"key", L"").empty())
            Pair_Click(nullptr, nullptr); // first start: the key from the PC, then Connect
        else
            Connect();
    });
}

void MainPage::ShowStatus(const std::wstring& text, bool timings)
{
    auto copy = ref new String(text.c_str());
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy, timings]()
    {
        if (!timings || m_showTimings)
            Status->Text = copy;
    }));
}

void MainPage::Connect()
{
    if (m_link)
        return;
    auto link = new Link();
    m_link = link;
    std::wstring address = Setting(L"server", PixelServer);
    size_t colon = address.find(L':');
    link->server = Utf8(address.substr(0, colon));
    if (colon != std::wstring::npos)
        link->port = _wtoi(address.c_str() + colon + 1);
    link->macKey = MacKey(Setting(L"key", L""));
    link->tailnet = address == PixelServer;
    link->tsOnly = link->tailnet && Setting(L"via", L"") == L"ts";
    if (link->tailnet)
    {
        std::lock_guard<std::mutex> lock(g_tailnetLock);
        if (!g_tailnet)
        {
            ts::setRandomSource(&WinRandom);
            ts::Tailnet::Options o;
            o.statePath = Utf8(std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\tailnet.state");
            o.authKey = Utf8(Setting(L"tskey", L""));
            o.hostname = "lumia-remoto";
            o.holePunch = Setting(L"punch", L"") == L"1";   // menu "attiva/disattiva hole punching" (PLAN §66)
            o.log = [](const std::string& line) { Log("ts: " + line); };
            o.onAuthUrl = [](const std::string& url)
            {
                {
                    std::lock_guard<std::mutex> lock(g_tailnetLock);
                    g_tailnetLogin = url;
                }
                Log("tailscale: login " + url);
            };
            g_tailnet = std::make_shared<ts::Tailnet>(o);
            g_tailnet->start();
        }
    }
    float scale = static_cast<float>(Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->RawPixelsPerViewPixel);
    int width = static_cast<int>(Screen->ActualWidth * scale + 0.5);
    int height = static_cast<int>(Screen->ActualHeight * scale + 0.5);
    MainPage^ page = this;
    link->video.log = [page, link](const std::string& line)
    {
        page->ShowStatus(Wide(line), true);
        link->SendText(line);
    };
    if (!link->video.CreateDevice(Screen, width, height, scale))
        return;
    ShowStatus(Wide(link->video.Describe()), true);
    bool askApps = !m_appsAsked;
    m_appsAsked = true;

    link->thread = std::thread([page, link, askApps]()
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        std::vector<unsigned char> unit;
        bool firstSession = true;
        while (!link->stop)
        {
            std::string where = link->server + ":" + std::to_string(link->port);
            auto c = std::make_shared<Conn>();
            // LAN first (1.5 s); for the Pixel, then Tailscale (ponte-pixel), which works anywhere.
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(link->port));
            address.sin_addr.s_addr = ParseIPv4(link->server);
            BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
            int buffer = 1 << 20;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
            page->ShowStatus(Wide("collegamento a " + where + "..."), false);
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address);
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval wait = { 1, 500000 };
            bool lan = !link->tsOnly && select(0, nullptr, &writable, &failed, &wait) == 1 && FD_ISSET(s, &writable);
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            std::string via = "LAN";
            if (lan)
                c->socket = s;
            else
            {
                closesocket(s);
                std::shared_ptr<ts::Tailnet> tailnet;
                {
                    std::lock_guard<std::mutex> lock(g_tailnetLock);
                    tailnet = g_tailnet;
                }
                if (!link->tailnet || !tailnet)
                {
                    page->ShowStatus(Wide("non raggiungo " + where), false);
                    Sleep(1000);
                    continue;
                }
                ++link->tsAttempts;
                std::string state = tailnet->status();
                Log("LAN non risponde, tentativo Tailscale " + std::to_string(link->tsAttempts) + " (" + state + ")");
                page->ShowStatus(Wide("fuori casa: tentativo Tailscale " + std::to_string(link->tsAttempts) + "... (" + state + ")"), false);
                std::string err;
                long long dialStart = NowUs();
                Log("dial " + std::string(TailnetPixel) + ":" + std::to_string(link->port) + "...");
                c->tcp = tailnet->dial(TailnetPixel, static_cast<uint16_t>(link->port), 20000, err);
                Log("dial finito in " + std::to_string((NowUs() - dialStart) / 1000) + " ms: " + (c->tcp ? "aperto" : err));
                if (!c->tcp)
                {
                    std::string login;
                    {
                        std::lock_guard<std::mutex> lock(g_tailnetLock);
                        login = g_tailnetLogin;
                    }
                    page->ShowStatus(Wide(login.empty() ? "Tailscale: " + err : "Tailscale da registrare: " + login), false);
                    Log("dial ponte-pixel: " + err + " | " + tailnet->status());
                    // no kick here: a slow first setup (control, DERP) would be thrown away at every attempt
                    Sleep(1000);
                    continue;
                }
                via = "Tailscale";
                Log("collegato via Tailscale");
            }
            if (link->stop)
            {
                c->Close();
                break;
            }
            {
                std::lock_guard<std::mutex> lock(link->sendLock);
                link->conn = c;
            }
            page->ShowStatus(L"", false);
            link->SendText("collegato via " + via);
            Log("collegato via " + via);
            // An empty display sends nothing (no app open, e.g. after the Pixel side restarted): show the list.
            long long connectedAt = NowUs();
            page->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([page, connectedAt]()
            {
                auto timer = ref new DispatcherTimer();
                TimeSpan two;
                two.Duration = 20000000;
                timer->Interval = two;
                timer->Tick += ref new EventHandler<Object^>([page, timer, connectedAt](Object^, Object^)
                {
                    timer->Stop();
                    if (page->m_lastVideo < connectedAt && page->m_link)
                        page->ShowAppList(true);
                });
                timer->Start();
            }));
            link->SendText(link->video.Describe());
            if (!link->audioTried)
            {
                link->audioTried = true;
                std::string err;
                link->audioReady = link->audio.Start(err);
                link->SendText(link->audioReady ? "audio: XAudio2 + Opus pronti" : "audio: " + err);
            }
            if (link->audioReady)
                link->Send('O', nullptr, 0); // the Pixel sends its sound only to clients that ask
            if (askApps && firstSession)
                link->Send('Q', nullptr, 0);
            {
                unsigned char held = static_cast<unsigned char>(page->m_rotation.load());
                link->Send('W', &held, 1);
            }
            firstSession = false;
            link->pinger = std::thread([link]()
            {
                while (!link->stop && link->Current())
                {
                    long long now = NowUs();
                    link->Send('p', &now, 8);
                    Sleep(500);
                }
            });
            link->statsAt = NowUs();
            link->blind = 0;
            for (;;)
            {
                char kind = 0;
                if (!c->ReceiveAll(&kind, 1))
                    break;
                if (kind == 'C')
                {
                    // the Pixel's challenge: HMAC of the nonce with the ponte key's MAC key
                    unsigned char nonce[16];
                    if (!c->ReceiveAll(nonce, 16))
                        break;
                    if (link->macKey.empty())
                    {
                        page->ShowStatus(L"manca la chiave del ponte (menu: scarica la chiave del ponte)", false);
                        break;
                    }
                    auto answer = Hmac(link->macKey, nonce, 16);
                    link->Send('A', answer.data(), answer.size());
                    continue;
                }
                if (kind == 'P')
                {
                    unsigned char body[16];
                    if (!c->ReceiveAll(body, 16))
                        break;
                    long long now = NowUs();
                    long long serverUs = Read<long long>(body);
                    long long sent = Read<long long>(body + 8);
                    long long rtt = now - sent;
                    link->rtt.push_back(rtt);
                    // The shortest round trip gives the best offset; the bound grows slowly so it follows drift.
                    link->bestRtt = link->bestRtt == LLONG_MAX ? rtt : link->bestRtt + 200;
                    if (rtt <= link->bestRtt)
                    {
                        link->bestRtt = rtt;
                        link->offset = serverUs - (sent + rtt / 2);
                        link->offsetKnown = true;
                    }
                    continue;
                }
                if (kind == 'U')
                {
                    unsigned char header[12];
                    if (!c->ReceiveAll(header, 12))
                        break;
                    unsigned int n = Read<unsigned int>(header);
                    if (n > 4096)
                        break;
                    unsigned char packet[4096];
                    if (n && !c->ReceiveAll(packet, n))
                        break;
                    link->audio.Packet(packet, n, NowUs(), Read<long long>(header + 4));
                    continue;
                }
                if (kind == 'k')
                {
                    // the Pixel's lock prompt is up / gone: {"on": bool}
                    unsigned int n = 0;
                    if (!c->ReceiveAll(&n, 4) || n > 4096)
                        break;
                    std::string json(n, '\0');
                    if (n && !c->ReceiveAll(&json[0], n))
                        break;
                    page->ShowPin(json.find("true") != std::string::npos);
                    continue;
                }
                if (kind == 'I' || kind == 'c')
                {
                    // 'I' a Pixel field wants the keyboard / is gone (JSON) | 'c' text copied on the Pixel
                    unsigned int n = 0;
                    if (!c->ReceiveAll(&n, 4) || n > (1 << 20))
                        break;
                    std::string body(n, '\0');
                    if (n && !c->ReceiveAll(&body[0], n))
                        break;
                    auto text = ref new String(Wide(body).c_str());
                    if (kind == 'I')
                        page->OnField(text);
                    else
                        page->OnCopied(text);
                    continue;
                }
                if (kind == 'm')
                {
                    unsigned char on = 0;
                    if (!c->ReceiveAll(&on, 1))
                        break;
                    page->OnMicWanted(on != 0);
                    continue;
                }
                if (kind == 'o')
                {
                    // the apps open on the Pixel's display: [{"t":task,"p":package,"l":label}]
                    unsigned int n = 0;
                    if (!c->ReceiveAll(&n, 4) || n > (1 << 20))
                        break;
                    std::string json(n, '\0');
                    if (n && !c->ReceiveAll(&json[0], n))
                        break;
                    page->ShowOpen(ref new String(Wide(json).c_str()));
                    continue;
                }
                if (kind == 'a')
                {
                    unsigned int n = 0;
                    if (!c->ReceiveAll(&n, 4) || n > (1 << 20))
                        break;
                    std::string json(n, '\0');
                    if (n && !c->ReceiveAll(&json[0], n))
                        break;
                    page->ShowApps(ref new String(Wide(json).c_str()));
                    continue;
                }
                if (kind != 'V')
                    break;
                unsigned char header[16];
                if (!c->ReceiveAll(header, 16))
                    break;
                unsigned int size = Read<unsigned int>(header);
                long long serverUs = Read<long long>(header + 4);
                unsigned int seq = Read<unsigned int>(header + 12);
                unit.resize(size);
                if (!c->ReceiveAll(unit.data(), size))
                    break;
                long long arrival = NowUs();
                // Acknowledged at once, before decoding: the Pixel paces its frames on these (RemotoStream) and
                // sends no more than the path carries, so nothing here ever has to be skipped to catch up. (The
                // old catch-up threw frames away and asked for key frames when they came late; through the relay
                // the big key frames made them later still: 10 resets in 40 s at 1 Mbit/s.)
                link->Send('F', &seq, 4);
                if (link->offsetKnown)
                    link->net.push_back(arrival + link->offset - serverUs);
                link->bytes += size + 17;
                ++link->frames;

                if (!link->decoderTried)
                {
                    link->decoderTried = true;
                    link->decoderReady = link->video.CreateDecoder(link->width, link->height);
                    if (!link->decoderReady)
                        page->ShowStatus(L"decoder H.264 non disponibile", false);
                }
                if (!link->decoderReady)
                    continue;
                page->m_lastVideo = arrival;
                bool newer = c->HasCompleteVideo();
                Video::Timing timing;
                bool pictured = link->video.Frame(unit.data(), unit.size(), static_cast<long long>(seq) * 166667, timing, !newer);
                if (pictured && !newer)
                {
                    link->decode.push_back(timing.decoded);
                    link->convert.push_back(timing.converted - timing.decoded);
                    link->present.push_back(timing.presented - timing.converted);
                }
                if (newer)
                    ++link->unshown;
                // The decoder lost the picture (a broken stream, a decoder reset): ask for a key frame, once a second.
                link->blind = pictured ? 0 : link->blind + 1;
                if (link->blind >= 10 && arrival - link->keyAsked > 1000000)
                {
                    link->keyAsked = arrival;
                    ++link->resets;
                    link->Send('K', nullptr, 0);
                    link->SendText("nessuna immagine dal decoder da 10 fotogrammi: chiedo un fotogramma chiave");
                }

                // Timings every 2 s on screen (when asked for); to the Pixel's log every 30 s while frames come.
                long long now = NowUs();
                if (now - link->statsAt >= 2000000)
                {
                    double seconds = (now - link->statsAt) / 1e6;
                    std::string path = "LAN";
                    if (c->tcp)
                    {
                        std::shared_ptr<ts::Tailnet> tailnet;
                        {
                            std::lock_guard<std::mutex> lock(g_tailnetLock);
                            tailnet = g_tailnet;
                        }
                        {
                            path = tailnet ? tailnet->pathTo(TailnetPixel) + (tailnet->holePunch() ? " (hole punching)" : "") : "Tailscale";
                            std::string nat = tailnet ? tailnet->natKind() : "";
                            if (!nat.empty())
                                path += ", " + nat;
                        }
                    }
                    char text[400];
                    sprintf_s(text, "%.1f fps | rete %s ms (rtt %s) | decode %s | conv %s | present %s | %.0f KB/s | non mostrati %d | chiave chiesti %d | audio %d ms, scartati %d | %s",
                              link->frames / seconds, Summary(link->net).c_str(), Summary(link->rtt).c_str(),
                              Summary(link->decode).c_str(), Summary(link->convert).c_str(), Summary(link->present).c_str(),
                              link->bytes / 1024.0 / seconds, link->unshown, link->resets, link->audio.TargetMs(), link->audio.Dropped(),
                              path.c_str());
                    page->ShowStatus(Wide(text), true);
                    if (now - link->statsSentAt >= 30000000)
                    {
                        link->SendText(text);
                        link->statsSentAt = now;
                    }
                    link->net.clear();
                    link->rtt.clear();
                    link->decode.clear();
                    link->convert.clear();
                    link->present.clear();
                    link->bytes = 0;
                    link->frames = 0;
                    link->statsAt = now;
                }
            }
            {
                std::lock_guard<std::mutex> lock(link->sendLock);
                link->conn = nullptr;
            }
            page->OnMicWanted(false);   // the Pixel's session (and its microphone policy) is gone
            Log("collegamento chiuso: " + c->Why() + (c->tcp ? " | " + c->tcp->stats() : ""));
            c->Close();
            if (link->pinger.joinable())
                link->pinger.join();
            if (!link->stop)
                page->ShowStatus(L"collegamento chiuso, riprovo...", false);
            Sleep(500);
        }
    });
}

// ------------------------------------------------------------------------------------------------------- input

// Touch on its own thread (CoreIndependentInputSource): the UI thread's layout never delays a finger.
void MainPage::StartInput()
{
    if (m_inputLoop)
        return;
    m_inputLoop = ThreadPool::RunAsync(ref new WorkItemHandler([this](IAsyncAction^)
    {
        m_input = Screen->CreateCoreIndependentInputSource(
            CoreInputDeviceTypes::Touch | CoreInputDeviceTypes::Mouse | CoreInputDeviceTypes::Pen);
        m_input->PointerPressed += ref new TypedEventHandler<Object^, PointerEventArgs^>(
            [this](Object^, PointerEventArgs^ e) { Touch(0, e); });
        m_input->PointerMoved += ref new TypedEventHandler<Object^, PointerEventArgs^>(
            [this](Object^, PointerEventArgs^ e) { Touch(2, e); });
        m_input->PointerReleased += ref new TypedEventHandler<Object^, PointerEventArgs^>(
            [this](Object^, PointerEventArgs^ e) { Touch(1, e); });
        m_input->PointerCaptureLost += ref new TypedEventHandler<Object^, PointerEventArgs^>(
            [this](Object^, PointerEventArgs^ e) { Touch(3, e); });
        m_input->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }), WorkItemPriority::High, WorkItemOptions::TimeSliced);
}

// 'T' action (0 down, 1 up, 2 move, 3 cancel), slot, x, y in the stream's pixels.
void MainPage::Touch(int action, PointerEventArgs^ args)
{
    auto link = m_link;
    auto point = args->CurrentPoint;
    int id = static_cast<int>(point->PointerId);
    int slot = -1;
    for (int i = 0; i < 10; ++i)
        if (m_pointerIds[i] == id)
            slot = i;
    if (action == 0 && slot < 0)
    {
        for (int i = 0; i < 10 && slot < 0; ++i)
            if (m_pointerIds[i] < 0)
                slot = i;
        if (slot < 0)
            return;
        m_pointerIds[slot] = id;
    }
    if (slot < 0)
        return;
    if (action == 2 && !point->IsInContact)
        return;
    if (action == 1 || action == 3)
        m_pointerIds[slot] = -1;
    if (!link || m_listOpen || m_menuOpen || m_openOpen || m_pinOpen || m_panelWidth <= 0 || m_panelHeight <= 0)
        return;
    float x = point->Position.X / m_panelWidth * link->width;
    float y = point->Position.Y / m_panelHeight * link->height;
    // Menu: a swipe up that starts near the bottom edge (last 60 px) and goes up 100 px. It reached the Pixel as a
    // normal touch until then, so the Pixel gets a cancel; the rest of that finger stays here.
    if (action == 0 && m_rotation != 0)
    {
        // check of the counter-rotated panel: the point must come in the panel's own (portrait) space
        char text[96];
        sprintf_s(text, "tocco ruotato %d: %.0f,%.0f nel pannello %.0fx%.0f", m_rotation.load() * 90,
                  point->Position.X, point->Position.Y, m_panelWidth, m_panelHeight);
        link->SendText(text);
    }
    if (action == 0)
    {
        m_startY[slot] = y;
        m_edge[slot] = false;
    }
    if (m_edge[slot])
        return;
    if (action == 2 && m_startY[slot] >= link->height - 60 && m_startY[slot] - y > 100)
    {
        m_edge[slot] = true;
        action = 3;
        Dispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([this]() { OpenMenu(); }));
    }
    unsigned char body[6];
    body[0] = static_cast<unsigned char>(action);
    body[1] = static_cast<unsigned char>(slot);
    unsigned short ux = static_cast<unsigned short>(std::max(0.0f, std::min(x, link->width - 1.0f)));
    unsigned short uy = static_cast<unsigned short>(std::max(0.0f, std::min(y, link->height - 1.0f)));
    memcpy(body + 2, &ux, 2);
    memcpy(body + 4, &uy, 2);
    link->Send('T', body, 6);
}

// Back: closes our own things first (app list, keyboard), else it is the Pixel's Back.
void MainPage::OnBack(Object^, BackPressedEventArgs^ e)
{
    e->Handled = true;
    if (m_pinOpen)
    {
        if (m_link)
            m_link->Send('Z', nullptr, 0); // cancels the Pixel's prompt; it closes ours when it is gone
        ShowPin(false);
        return;
    }
    if (m_menuOpen)
    {
        CloseMenu();
        return;
    }
    if (m_listOpen)
    {
        ShowAppList(false);
        return;
    }
    if (m_openOpen)
    {
        ShowOpenPanel(false);
        return;
    }
    if (m_typing)
    {
        Typing->IsEnabled = false; // loses the focus: the keyboard closes
        return;
    }
    if (m_link)
        m_link->Key(4); // KEYCODE_BACK
}

void MainPage::OpenMenu()
{
    m_menuOpen = true;
    MenuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::CloseMenu()
{
    m_menuOpen = false;
    MenuPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::MenuPanel_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    CloseMenu();
}

void MainPage::MenuBox_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true; // a tap between the buttons does not close the menu
}

// ------------------------------------------------------------------------------------------- the Pixel's PIN

void MainPage::ShowPin(bool on)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([this, on]()
    {
        if (on && !m_pinOpen)
            m_pinTries = 0;
        m_pinOpen = on;
        m_pin.clear();
        ShowPinText();
        PinPanel->Visibility = on ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
        if (on)
        {
            CloseMenu();
            ShowAppList(false);
            Typing->IsEnabled = false;
        }
    }));
}

void MainPage::ShowPinText()
{
    PinDots->Text = ref new String(std::wstring(m_pin.size(), L'\u25CF').c_str());
    PinTitle->Text = m_pinTries ? L"PIN NON ACCETTATO? RIPROVA" : L"PIN DEL PIXEL";
}

void MainPage::PinKey_Click(Object^ sender, RoutedEventArgs^)
{
    std::wstring key = static_cast<String^>(static_cast<Windows::UI::Xaml::Controls::Button^>(sender)->Content)->Data();
    if (key == L"OK")
    {
        if (m_pin.empty() || !m_link)
            return;
        m_link->SendBlock('Y', Utf8(m_pin));
        m_pin.clear();
        ++m_pinTries; // shown only if the prompt is still up: a right PIN closes it ('k' off)
        PinDots->Text = L"";
        auto self = this;
        Windows::System::Threading::ThreadPoolTimer::CreateTimer(ref new Windows::System::Threading::TimerElapsedHandler(
            [self](Windows::System::Threading::ThreadPoolTimer^)
        {
            self->Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self]()
            {
                if (self->m_pinOpen)
                    self->ShowPinText();
            }));
        }), Windows::Foundation::TimeSpan{ 15000000 });
        return;
    }
    if (key.size() == 1 && key[0] >= L'0' && key[0] <= L'9')
    {
        if (m_pin.size() < 16)
            m_pin += key;
    }
    else if (!m_pin.empty())
        m_pin.pop_back();
    PinDots->Text = ref new String(std::wstring(m_pin.size(), L'\u25CF').c_str());
}

// ------------------------------------------------------------------------------------------ rotation

// How the page is turned (Windows decides, so the system's rotation lock holds) -> 'W' to the Pixel, and the
// panel counter-rotated so that the phone's pixels stay the stream's. For a portrait phone "Landscape" = turned
// clockwise (the DirectX template's ROTATE270): the panel's top must point to the window's right, +90 degrees;
// Android calls that ROTATION_270. LandscapeFlipped = counter-clockwise, -90, ROTATION_90.
void MainPage::Orient()
{
    using namespace Windows::Graphics::Display;
    auto bounds = Window::Current->Bounds;
    float w = std::min(bounds.Width, bounds.Height), h = std::max(bounds.Width, bounds.Height);
    Screen->Width = w;
    Screen->Height = h;
    Canvas::SetLeft(Screen, (bounds.Width - w) / 2);
    Canvas::SetTop(Screen, (bounds.Height - h) / 2);
    int r = 0;
    double angle = 0;
    switch (DisplayInformation::GetForCurrentView()->CurrentOrientation)
    {
    case DisplayOrientations::Landscape: r = 3; angle = 90; break;
    case DisplayOrientations::LandscapeFlipped: r = 1; angle = -90; break;
    default: break;
    }
    ScreenTurn->Angle = angle;
    if (r != m_rotation.exchange(r))
    {
        if (m_link)
            m_link->SendText("Lumia: orientamento " + std::to_string(r * 90));
        SendRotation();
    }
}

void MainPage::SendRotation()
{
    if (auto link = m_link)
    {
        unsigned char r = static_cast<unsigned char>(m_rotation.load());
        link->Send('W', &r, 1);
    }
}

// ------------------------------------------------------------------------------------------ open apps

void MainPage::OpenApps_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    OpenList->Children->Clear();
    ShowOpenPanel(true);
    if (m_link)
        m_link->Send('q', nullptr, 0);
}

// Like the app list, the Pixel's picture is hidden and touches stay here while the panel is up.
void MainPage::ShowOpenPanel(bool show)
{
    m_openOpen = show;
    OpenPanel->Visibility = show ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    Screen->Opacity = show || m_listOpen ? 0 : 1;
}

void MainPage::CloseAll_Click(Object^, RoutedEventArgs^)
{
    if (m_link)
    {
        unsigned int all = 0;
        m_link->Send('x', &all, 4);
    }
    // nothing left to show on the Pixel's display: the app list
    ShowOpenPanel(false);
    ShowAppList(true);
    if (m_link)
        m_link->Send('Q', nullptr, 0);
}

void MainPage::ShowOpen(String^ json)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, json]()
    {
        Windows::Data::Json::JsonArray^ list = nullptr;
        if (!m_openOpen || !Windows::Data::Json::JsonArray::TryParse(json, &list))
            return;
        OpenList->Children->Clear();
        CloseAllButton->Visibility = list->Size ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
        if (!list->Size)
        {
            auto none = ref new TextBlock();
            none->Text = L"nessuna app aperta";
            none->FontSize = 22;
            none->Foreground = ref new Windows::UI::Xaml::Media::SolidColorBrush(Windows::UI::Colors::Gray);
            OpenList->Children->Append(none);
            return;
        }
        for (unsigned int i = 0; i < list->Size; ++i)
        {
            auto app = list->GetObjectAt(i);
            unsigned int task = static_cast<unsigned int>(app->GetNamedNumber(L"t"));
            std::wstring package = app->GetNamedString(L"p")->Data();
            auto row = ref new Grid();
            row->Margin = Thickness(0, 4, 0, 4);
            auto wide = ref new ColumnDefinition();
            wide->Width = GridLength(1, GridUnitType::Star);
            row->ColumnDefinitions->Append(wide);
            row->ColumnDefinitions->Append(ref new ColumnDefinition());
            row->ColumnDefinitions->GetAt(1)->Width = GridLength::Auto;
            auto name = ref new TextBlock();
            name->Text = app->GetNamedString(L"l");
            name->FontSize = 26;
            name->FontWeight = Windows::UI::Text::FontWeights::Light;
            name->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
            name->TextTrimming = TextTrimming::WordEllipsis;
            name->Tapped += ref new Windows::UI::Xaml::Input::TappedEventHandler([this, package](Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
            {
                ShowOpenPanel(false);
                OpenApp(package, false); // its task comes to the front
            });
            auto close = ref new Button();
            close->Content = L"chiudi";
            close->MinWidth = 0;
            close->Margin = Thickness(12, 0, 0, 0);
            close->FontSize = 18;
            Grid::SetColumn(close, 1);
            close->Click += ref new RoutedEventHandler([this, task, row](Object^, RoutedEventArgs^)
            {
                if (m_link)
                    m_link->Send('x', &task, 4);
                unsigned int at = 0;
                if (OpenList->Children->IndexOf(row, &at))
                    OpenList->Children->RemoveAt(at); // at once; the Pixel's fresh list follows
            });
            row->Children->Append(name);
            row->Children->Append(close);
            OpenList->Children->Append(row);
        }
    }));
}

// ------------------------------------------------------------------------- keyboard, clipboard, microphone

// 'I' (PLAN §68): the Pixel's keyboard (RemotoIme) says a field on Remoto's display wants input. The Lumia keyboard
// opens by itself with a fitting layout; when the field goes, it closes, unless another field opens within 300 ms
// (moving from one field to the next is "gone" then "wants").
void MainPage::OnField(String^ json)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([this, json]()
    {
        Windows::Data::Json::JsonObject^ o = nullptr;
        if (!Windows::Data::Json::JsonObject::TryParse(json, &o))
            return;
        int seq = ++m_fieldSeq;
        bool on = o->HasKey(L"on") && o->GetNamedBoolean(L"on");
        if (!on)
        {
            if (!m_fieldOpen)
                return;
            auto timer = ref new DispatcherTimer();
            TimeSpan wait;
            wait.Duration = 3000000;
            timer->Interval = wait;
            timer->Tick += ref new EventHandler<Object^>([this, timer, seq](Object^, Object^)
            {
                timer->Stop();
                if (m_fieldSeq != seq || !m_fieldOpen)
                    return;
                m_fieldOpen = false;
                if (m_typing)
                    Typing->IsEnabled = false; // loses the focus: the keyboard closes
            });
            timer->Start();
            return;
        }
        if (m_listOpen || m_menuOpen || m_openOpen || m_pinOpen)
            return;
        std::wstring type = o->HasKey(L"type") ? o->GetNamedString(L"type")->Data() : L"text";
        std::wstring action = o->HasKey(L"action") ? o->GetNamedString(L"action")->Data() : L"none";
        using Windows::UI::Xaml::Input::InputScopeNameValue;
        InputScopeNameValue value = InputScopeNameValue::Default;
        if (type == L"number" || type == L"pin")
            value = InputScopeNameValue::Number;
        else if (type == L"phone")
            value = InputScopeNameValue::TelephoneNumber;
        else if (type == L"email")
            value = InputScopeNameValue::EmailSmtpAddress;
        else if (type == L"url")
            value = InputScopeNameValue::Url;
        else if (action == L"search")
            value = InputScopeNameValue::Search;
        auto scope = ref new Windows::UI::Xaml::Input::InputScope();
        scope->Names->Append(ref new Windows::UI::Xaml::Input::InputScopeName(value));
        m_fieldOpen = true;
        if (m_typing)
            return; // the keyboard is up already (its layout stays until it closes)
        Typing->InputScope = scope;
        Typing->IsEnabled = true;
        Typing->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    }));
}

// 'c': copied on the Pixel. A bar for 8 s; a tap shows the text selected, the system's copy button copies it here.
void MainPage::OnCopied(String^ text)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, text]()
    {
        m_copied = text;
        int seq = ++m_copySeq;
        std::wstring preview = text->Data();
        for (auto& ch : preview)
            if (ch == L'\r' || ch == L'\n' || ch == L'\t')
                ch = L' ';
        if (preview.size() > 60)
            preview = preview.substr(0, 60) + L"\u2026";
        CopyText->Text = ref new String((L"Copiato sul Pixel \u00AB" + preview + L"\u00BB \u2014 tocca per copiarlo qui").c_str());
        CopyBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
        auto timer = ref new DispatcherTimer();
        TimeSpan wait;
        wait.Duration = 80000000;
        timer->Interval = wait;
        timer->Tick += ref new EventHandler<Object^>([this, timer, seq](Object^, Object^)
        {
            timer->Stop();
            if (m_copySeq == seq)
                CopyBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        });
        timer->Start();
    }));
}

void MainPage::CopyBar_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    CopyBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    if (!m_copied)
        return;
    // the XAML TextBox (RichEdit inside) breaks lines on \r and cuts the text at a bare \n (Telegram's Copia)
    std::wstring text;
    const wchar_t* src = m_copied->Data();
    for (unsigned i = 0; i < m_copied->Length(); ++i)
    {
        if (src[i] == L'\r') { if (i + 1 < m_copied->Length() && src[i + 1] == L'\n') ++i; text += L'\r'; }
        else if (src[i] == L'\n') text += L'\r';
        else text += src[i];
    }
    auto box = ref new TextBox();
    box->AcceptsReturn = true;
    box->TextWrapping = TextWrapping::Wrap;
    box->Text = ref new String(text.c_str(), static_cast<unsigned>(text.size()));
    box->IsReadOnly = true;
    box->MaxHeight = 420;
    ScrollViewer::SetVerticalScrollBarVisibility(box, ScrollBarVisibility::Auto);
    box->Loaded += ref new RoutedEventHandler([box](Object^, RoutedEventArgs^)
    {
        box->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        box->SelectAll();
    });
    auto dialog = ref new ContentDialog();
    dialog->Title = L"Copia";
    dialog->Content = box;
    dialog->PrimaryButtonText = L"Chiudi";
    m_listOpen = true; // touches stay here while the dialog is up
    create_task(dialog->ShowAsync()).then([this](task<ContentDialogResult> t)
    {
        try { t.get(); } catch (Exception^) { }
        m_listOpen = AppsPanel->Visibility == Windows::UI::Xaml::Visibility::Visible;
    }, task_continuation_context::use_current());
}

// "incolla dal Lumia": WP8.1 apps cannot read the clipboard, but the keyboard's paste button can put it in a box;
// "invia" types it into the Pixel's field (commitText through RemotoIme when a field is open).
void MainPage::Paste_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    auto box = ref new TextBox();
    box->AcceptsReturn = true;
    box->TextWrapping = TextWrapping::Wrap;
    box->MaxHeight = 300;
    box->PlaceholderText = L"tocca l'icona incolla sopra la tastiera";
    box->Loaded += ref new RoutedEventHandler([box](Object^, RoutedEventArgs^)
    {
        box->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    });
    auto dialog = ref new ContentDialog();
    dialog->Title = L"Incolla sul Pixel";
    dialog->Content = box;
    dialog->PrimaryButtonText = L"invia";
    dialog->SecondaryButtonText = L"annulla";
    m_listOpen = true;
    create_task(dialog->ShowAsync()).then([this, box](task<ContentDialogResult> t)
    {
        ContentDialogResult r = ContentDialogResult::None;
        try { r = t.get(); } catch (Exception^) { }
        m_listOpen = AppsPanel->Visibility == Windows::UI::Xaml::Visibility::Visible;
        std::wstring text = box->Text->Data();
        for (auto& ch : text)
            if (ch == L'\r')
                ch = L'\n';
        if (r == ContentDialogResult::Primary && !text.empty() && m_link)
            m_link->SendBlock('X', Utf8(text));
    }, task_continuation_context::use_current());
}

// 'm': an app on the Pixel records from its microphone, which is now ours (RemotoMic). Any thread.
void MainPage::OnMicWanted(bool on)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([this, on]()
    {
        m_micWanted = on;
        MicBadge->Visibility = on ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
        if (!on)
        {
            Mic::Stop();
            return;
        }
        if (!m_micId.empty() || !Setting(L"microfono", L"").empty())
        {
            if (m_micId.empty())
                m_micId = Setting(L"microfono", L"");
            StartMic();
            return;
        }
        // The Lumia 1020 records only from the "Enhanced Audio Recording Device"; a headset or a Bluetooth one wins
        // (apps/Assistente FindMicrophone). Looked up once, kept in the settings.
        using namespace Windows::Devices::Enumeration;
        create_task(DeviceInformation::FindAllAsync(Windows::Media::Devices::MediaDevice::GetAudioCaptureSelector()))
            .then([this](task<DeviceInformationCollection^> t)
        {
            std::wstring enhanced, headset, bluetooth;
            try
            {
                for (auto d : t.get())
                {
                    if (!d->IsEnabled)
                        continue;
                    std::wstring name = d->Name->Data();
                    if (name.find(L"QCAUD") == std::wstring::npos || name.find(L"Bluetooth") != std::wstring::npos
                        || name.find(L"Hands-Free") != std::wstring::npos)
                        bluetooth = d->Id->Data();
                    else if (name.compare(0, 18, L"Headset Microphone") == 0)
                        headset = d->Id->Data();
                    else if (name.find(L"Enhanced") != std::wstring::npos)
                        enhanced = d->Id->Data();
                }
            }
            catch (Exception^) { }
            m_micId = !bluetooth.empty() ? bluetooth : headset.empty() ? enhanced : headset;
            if (!enhanced.empty())
                Settings()->Insert(L"microfono", ref new String(enhanced.c_str()));
            if (m_micWanted)
                StartMic();
        }, task_continuation_context::use_current());
    }));
}

void MainPage::StartMic()
{
    MainPage^ page = this;
    Mic::Start(m_micId, [page](const unsigned char* packet, size_t size)
    {
        auto link = page->m_link;
        if (!link)
            return;
        std::string body(12 + size, '\0');
        unsigned int n = static_cast<unsigned int>(size);
        long long now = NowUs();
        memcpy(&body[0], &n, 4);
        memcpy(&body[4], &now, 8);
        memcpy(&body[12], packet, size);
        link->Send('M', body.data(), body.size());
    }, [page](const std::string& line)
    {
        Log(line);
        if (auto link = page->m_link)
            link->SendText(line);
    });
}

void MainPage::Keyboard_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    Typing->IsEnabled = true;
    Typing->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

// ------------------------------------------------------------------------------------------------------- apps

void MainPage::Apps_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    ShowAppList(true);
    if (m_link)
        m_link->Send('Q', nullptr, 0); // fresh list (apps installed meanwhile)
}

// While the list is up the Pixel's picture is hidden (no composition of a 60 fps swap chain under a scrolling list)
// and touches are not sent.
void MainPage::ShowAppList(bool show)
{
    m_listOpen = show;
    AppsPanel->Visibility = show ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    Screen->Opacity = show ? 0 : 1;
    if (!show)
        Search->Text = L"";
}

void MainPage::ShowApps(String^ json)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, json]()
    {
        Windows::Data::Json::JsonArray^ list = nullptr;
        if (!Windows::Data::Json::JsonArray::TryParse(json, &list))
            return;
        m_allLabels.clear();
        m_allPackages.clear();
        for (unsigned int i = 0; i < list->Size; ++i)
        {
            auto app = list->GetObjectAt(i);
            std::wstring label = app->GetNamedString(L"l")->Data();
            std::wstring package = app->GetNamedString(L"p")->Data();
            if (std::find(m_allLabels.begin(), m_allLabels.end(), label) != m_allLabels.end())
                label += L" (" + package + L")"; // two apps with one name: items are found by label
            m_allLabels.push_back(label);
            m_allPackages.push_back(package);
        }
        Filter();
        // first list of this run with no app started yet: show it
        if (Setting(L"last", L"").empty())
            ShowAppList(true);
    }));
}

// The search box: every word must be in the name (case-insensitive).
void MainPage::Filter()
{
    std::wstring query = Search->Text->Data();
    std::transform(query.begin(), query.end(), query.begin(), towlower);
    std::vector<std::wstring> words;
    size_t at = 0;
    while (at < query.size())
    {
        size_t end = query.find(L' ', at);
        if (end == std::wstring::npos)
            end = query.size();
        if (end > at)
            words.push_back(query.substr(at, end - at));
        at = end + 1;
    }
    m_appLabels->Clear();
    m_appPackages.clear();
    for (size_t i = 0; i < m_allLabels.size(); ++i)
    {
        std::wstring name = m_allLabels[i];
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        bool all = true;
        for (auto& w : words)
            if (name.find(w) == std::wstring::npos)
                all = false;
        if (!all)
            continue;
        m_appLabels->Append(ref new String(m_allLabels[i].c_str()));
        m_appPackages.push_back(m_allPackages[i]);
    }
}

int MainPage::PackageOf(String^ label)
{
    unsigned int index = 0;
    if (!label || !m_appLabels->IndexOf(label, &index) || index >= m_appPackages.size())
        return -1;
    return static_cast<int>(index);
}

void MainPage::OpenApp(const std::wstring& package, bool restart)
{
    Settings()->Insert(L"last", ref new String(package.c_str()));
    ShowAppList(false);
    if (m_link)
        m_link->SendBlock(restart ? 'R' : 'L', Utf8(package));
}

void MainPage::AppList_ItemClick(Object^, ItemClickEventArgs^ e)
{
    int index = PackageOf(safe_cast<String^>(e->ClickedItem));
    if (index >= 0)
        OpenApp(m_appPackages[index], false);
}

// Hold on an app: open it, or restart it (force-stop on the Pixel, then start).
void MainPage::AppItem_Holding(Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    auto item = safe_cast<FrameworkElement^>(sender);
    int index = PackageOf(dynamic_cast<String^>(item->DataContext));
    if (index < 0)
        return;
    std::wstring package = m_appPackages[index];
    auto menu = ref new MenuFlyout();
    auto open = ref new MenuFlyoutItem();
    open->Text = L"apri";
    open->Click += ref new RoutedEventHandler([this, package](Object^, RoutedEventArgs^) { OpenApp(package, false); });
    auto restart = ref new MenuFlyoutItem();
    restart->Text = L"riavvia";
    restart->Click += ref new RoutedEventHandler([this, package](Object^, RoutedEventArgs^) { OpenApp(package, true); });
    menu->Items->Append(open);
    menu->Items->Append(restart);
    menu->ShowAt(item);
}

// The menu's "riavvia l'app": the last app opened from the list.
void MainPage::Restart_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    std::wstring last = Setting(L"last", L"");
    if (!last.empty() && m_link)
        m_link->SendBlock('R', Utf8(last));
}

// ------------------------------------------------------------------------------------------------------- menu

void MainPage::Stats_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    m_showTimings = !m_showTimings;
    StatsButton->Content = m_showTimings ? L"nascondi i tempi" : L"mostra i tempi";
    if (!m_showTimings)
        Status->Text = L"";
}

void MainPage::Reconnect_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    Suspend();
    Connect();
}

void MainPage::TsOnly_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    bool on = Setting(L"via", L"") != L"ts";
    Settings()->Insert(L"via", on ? L"ts" : L"");
    UpdateServerButton();
    Reconnect_Click(nullptr, nullptr);
}

// Hole punching (STUN + CallMeMaybe): a direct UDP path between the Lumia on 4G and the Pixel at home, no relay.
// Off by default; the stats line says "diretto a.b.c.d:port" when it works, "relay" when it does not.
void MainPage::Punch_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    bool on = Setting(L"punch", L"") != L"1";
    Settings()->Insert(L"punch", on ? L"1" : L"");
    std::shared_ptr<ts::Tailnet> tailnet;
    {
        std::lock_guard<std::mutex> lock(g_tailnetLock);
        tailnet = g_tailnet;
    }
    if (tailnet)
        tailnet->setHolePunch(on);
    UpdateServerButton();
    Reconnect_Click(nullptr, nullptr);
}

void MainPage::UpdateServerButton()
{
    PunchButton->Content = Setting(L"punch", L"") == L"1" ? L"disattiva hole punching" : L"attiva hole punching";
    TsOnlyButton->Content = Setting(L"via", L"") == L"ts" ? L"LAN e Tailscale" : L"solo Tailscale";
    bool bench = Setting(L"server", PixelServer) == BenchServer;
    ServerButton->Content = bench ? L"usa il Pixel" : L"usa il banco del PC";
}

void MainPage::Server_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    bool bench = Setting(L"server", PixelServer) == BenchServer;
    Settings()->Insert(L"server", StringReference(bench ? PixelServer : BenchServer));
    UpdateServerButton();
    Reconnect_Click(nullptr, nullptr);
}

// The ponte key from the PC share (ponte-pair.txt "<host> <key>", one line).
void MainPage::Pair_Click(Object^, RoutedEventArgs^)
{
    CloseMenu();
    Status->Text = L"scarico la chiave del ponte...";
    auto http = ref new Windows::Web::Http::HttpClient();
    create_task(http->GetStringAsync(ref new Uri(StringReference(PairUrl)))).then([this, http](task<String^> t)
    {
        std::wstring text;
        try { text = t.get()->Data(); } catch (Exception^) { }
        // "<host> <key> [tskey-auth-...]"
        std::wstring key, tskey;
        size_t space = text.find(L' ');
        if (space != std::wstring::npos)
        {
            size_t end = text.find_first_of(L" \r\n", space + 1);
            for (wchar_t c : text.substr(space + 1, end == std::wstring::npos ? std::wstring::npos : end - space - 1))
                if (iswxdigit(c))
                    key += static_cast<wchar_t>(towlower(c));
            size_t ts = text.find(L"tskey-");
            if (ts != std::wstring::npos)
                tskey = text.substr(ts, text.find_first_of(L" \r\n", ts) - ts);
        }
        if (!tskey.empty())
            Settings()->Insert(L"tskey", ref new String(tskey.c_str()));
        if (key.size() == 64)
        {
            Settings()->Insert(L"key", ref new String(key.c_str()));
            Status->Text = L"chiave del ponte salvata";
        }
        else
            Status->Text = L"chiave del ponte non scaricata (tools/inbox acceso sul PC?)";
        Reconnect_Click(nullptr, nullptr);
    }, task_continuation_context::use_current());
}

void MainPage::Suspend()
{
    Mic::Stop();
    auto link = m_link;
    m_link = nullptr;
    if (!link)
        return;
    link->stop = true;
    if (auto c = link->Current())
        c->Close();
    // The thread ends on the closed socket; the link is left to it (a few KB) instead of waiting here.
    if (link->thread.joinable())
        link->thread.detach();
}

void MainPage::Resume()
{
    {
        std::lock_guard<std::mutex> lock(g_tailnetLock);
        if (g_tailnet)
            g_tailnet->kick(); // after a suspend its sessions are stale: they reconnect at once
    }
    Connect();
}
