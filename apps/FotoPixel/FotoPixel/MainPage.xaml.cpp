#include "pch.h"
#include "MainPage.xaml.h"
#include "PixelToken.h"
#include "tailnet.h"

#include <windows.h>
#include <condition_variable>
#include <fstream>
#include <atomic>
#include <thread>

using namespace FotoPixel;
using namespace concurrency;
using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Cryptography;
using namespace Windows::Security::Cryptography::Core;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Streams;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::Web::Http;
using namespace Windows::Web::Http::Filters;
using namespace Windows::UI::Popups;

namespace
{
    const unsigned CHUNK = 1u << 20;      // 1 MB per PUT /chunk
    const int PORT = 8792, UDP_PORT = 8793;
    const wchar_t* DEFAULT_HOST = L"192.168.1.10";
    const char* TS_HOST = "pixel-foto";          // the Pixel's node in the tailnet (PLAN.md 58.3)
    const char* TS_SELF = "lumia-foto";
    std::mutex logMu;

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }

    String^ S(const std::wstring& w) { return ref new String(w.c_str()); }

    bool EndsWithI(const std::wstring& s, const std::wstring& tail)
    {
        if (s.size() < tail.size()) return false;
        return _wcsicmp(s.c_str() + s.size() - tail.size(), tail.c_str()) == 0;
    }

    std::wstring Lower(std::wstring s)
    {
        for (auto& c : s) c = towlower(c);
        return s;
    }

    String^ Mb(unsigned long long bytes)
    {
        wchar_t b[32];
        swprintf_s(b, L"%.1f MB", bytes / 1048576.0);
        return ref new String(b);
    }

    // FILETIME-style 100 ns ticks since 1601 -> ms since 1970
    long long ToUnixMs(DateTime t) { return (t.UniversalTime - 116444736000000000LL) / 10000; }

    struct NetError { std::wstring what; };
}

std::string FotoPixel::Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void FotoPixel::Log(const std::string& line)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    sprintf_s(stamp, "%02d-%02d %02d:%02d:%02d.%03d ", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    OutputDebugStringA((stamp + line + "\n").c_str());
    std::lock_guard<std::mutex> lock(logMu);
    std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\run.log";
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << stamp << line << "\n";
}

MainPage::MainPage() : busy_(false), loading_(false), viaTs_(false)
{
    InitializeComponent();
    items_ = ref new Vector<Object^>();
    Photos->ItemsSource = items_;
    auto filter = ref new HttpBaseProtocolFilter();
    filter->CacheControl->ReadBehavior = HttpCacheReadBehavior::MostRecent;
    filter->CacheControl->WriteBehavior = HttpCacheWriteBehavior::NoCache;
    client_ = ref new HttpClient(filter);
    display_ = ref new Windows::System::Display::DisplayRequest();
    auto saved = dynamic_cast<String^>(ApplicationData::Current->LocalSettings->Values->Lookup(L"host"));
    host_ = saved != nullptr ? saved->Data() : DEFAULT_HOST;
}

void MainPage::OnNavigatedTo(Navigation::NavigationEventArgs^)
{
    if (items_->Size == 0 && !loading_)
    {
        loading_ = true;
        StartTailnet();
        ThreadPool::RunAsync(ref new WorkItemHandler([this](IAsyncAction^) { Load(); }));
    }
}

void MainPage::Resume()
{
    {
        std::lock_guard<std::mutex> lock(tsMu_);
        if (tsConn_) { tsConn_->close(); tsConn_.reset(); }
    }
    if (tailnet_) tailnet_->kick();
}

// ---- Tailscale ----

void MainPage::StartTailnet()
{
    if (tailnet_) return;
    ts::Tailnet::Options o;
    o.statePath = Narrow(std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\tailnet.state");
    o.hostname = TS_SELF;
    o.log = [](const std::string& line) { Log("ts: " + line); };
    o.onAuthUrl = [this](const std::string& url)
    {
        Log("tailscale: serve il login " + url);
        std::lock_guard<std::mutex> lock(tsMu_);
        tsAuthUrl_ = url;
    };
    tailnet_ = std::make_shared<ts::Tailnet>(o);
    tailnet_->start();
}

std::wstring MainPage::TsText()
{
    if (!tailnet_) return L"Tailscale: spento";
    std::wstring t = L"Tailscale (" + Widen(TS_SELF) + L"): " + Widen(tailnet_->status());
    std::string url;
    {
        std::lock_guard<std::mutex> lock(tsMu_);
        url = tsAuthUrl_;
    }
    if (!url.empty() && !tailnet_->ready()) t += L"\n\nServe il login (una volta sola): " + Widen(url);
    return t;
}

void MainPage::OnTailscale(Object^, RoutedEventArgs^)
{
    std::string url;
    {
        std::lock_guard<std::mutex> lock(tsMu_);
        url = tsAuthUrl_;
    }
    auto dlg = ref new MessageDialog(S(TsText()), "Tailscale");
    if (!url.empty() && tailnet_ && !tailnet_->ready())
    {
        String^ u = S(Widen(url));
        dlg->Commands->Append(ref new UICommand("apri il link", ref new UICommandInvokedHandler([u](IUICommand^)
        {
            Windows::System::Launcher::LaunchUriAsync(ref new Uri(u));
        })));
    }
    dlg->Commands->Append(ref new UICommand("ok"));
    dlg->ShowAsync();
}

bool MainPage::TsPing()
{
    if (!tailnet_) return false;
    // the netmap arrives a few seconds after start
    for (int i = 0; i < 20 && !tailnet_->ready(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!tailnet_->ready()) return false;
    try
    {
        Resp r = TsCall("GET", "/ping", nullptr, false, 30000);
        return r.status == 200;
    }
    catch (NetError& e)
    {
        Log("tailscale ping: " + Narrow(e.what));
        return false;
    }
}

MainPage::Resp MainPage::TsCall(const std::string& method, const std::string& path, IBuffer^ body, bool json,
    int timeoutMs)
{
    std::lock_guard<std::mutex> lock(tsMu_);
    Array<unsigned char>^ bytes = nullptr;
    size_t len = 0;
    if (body != nullptr && body->Length > 0)
    {
        CryptographicBuffer::CopyToByteArray(body, &bytes);
        len = bytes->Length;
    }
    std::string head = method + " " + path + " HTTP/1.1\r\nHost: " + TS_HOST + "\r\nX-Token: " + Narrow(PIXEL_TOKEN)
        + "\r\nContent-Length: " + std::to_string(len) + (json ? "\r\nContent-Type: application/json" : "") + "\r\n\r\n";
    std::string err;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        bool fresh = false;
        if (!tsConn_ || !tsConn_->isOpen())
        {
            if (tsConn_) tsConn_->close();
            tsConn_ = tailnet_->dial(TS_HOST, (uint16_t)PORT, 30000, err);
            if (!tsConn_) throw NetError{ L"Tailscale: " + Widen(err) };
            fresh = true;
        }
        auto c = tsConn_;
        ULONGLONG w0 = GetTickCount64();
        bool ok = c->write(head) && (len == 0 || c->write(bytes->Data, len));
        if (len > 100000) Log("ts " + method + " " + std::to_string(len) + " byte in coda in " + std::to_string(GetTickCount64() - w0)
            + " ms (" + c->stats() + ")");
        if (!ok) { err = "scrittura fallita"; Log("ts: scrittura fallita (" + c->stats() + ")"); }
        std::string in;
        size_t hdrEnd = std::string::npos;
        long long deadline = (long long)GetTickCount64() + timeoutMs;
        while (ok && hdrEnd == std::string::npos)
        {
            uint8_t buf[4096];
            int left = (int)(deadline - (long long)GetTickCount64());
            int n = left > 0 ? c->read(buf, sizeof buf, left) : -1;
            if (n <= 0)
            {
                ok = false;
                err = n < 0 ? "il Pixel non risponde" : "connessione chiusa";
                Log("ts " + method + " " + path.substr(0, 40) + ": " + err + " (" + c->stats() + ")");
                break;
            }
            in.append((const char*)buf, n);
            hdrEnd = in.find("\r\n\r\n");
        }
        if (ok)
        {
            Resp out;
            out.status = 0;
            out.json = nullptr;
            if (in.size() > 12) out.status = atoi(in.c_str() + 9);
            size_t cl = 0;
            std::string lower = in.substr(0, hdrEnd);
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            size_t k = lower.find("content-length:");
            if (k != std::string::npos) cl = (size_t)atoll(lower.c_str() + k + 15);
            std::string bodyIn = in.substr(hdrEnd + 4);
            while (bodyIn.size() < cl)
            {
                uint8_t buf[4096];
                int left = (int)(deadline - (long long)GetTickCount64());
                int n = left > 0 ? c->read(buf, sizeof buf, left) : -1;
                if (n <= 0) { ok = false; err = "risposta interrotta"; break; }
                bodyIn.append((const char*)buf, n);
            }
            if (ok)
            {
                JsonObject^ j;
                if (JsonObject::TryParse(S(Widen(bodyIn)), &j)) out.json = j;
                if (lower.find("connection: close") != std::string::npos) { c->close(); tsConn_.reset(); }
                return out;
            }
        }
        c->close();
        tsConn_.reset();
        if (fresh) break;   // a reused connection may just have gone stale: one more try on a new one
    }
    throw NetError{ L"Tailscale: " + Widen(err) };
}

bool MainPage::HandleBack()
{
    if (Photos->SelectedItems->Size > 0)
    {
        Photos->SelectedItems->Clear();
        return true;
    }
    return false;
}

// ---- UI helpers ----

void MainPage::Ui(std::function<void()> fn)
{
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([fn]() { fn(); }));
}

void MainPage::SetStatus(String^ text)
{
    Ui([this, text]() { Status->Text = text; });
}

void MainPage::UpdateButtons()
{
    unsigned n = Photos->SelectedItems->Size;
    SendButton->IsEnabled = n > 0 && !busy_;
    ClearButton->IsEnabled = n > 0;
    SendButton->Label = n > 1 ? "invia " + S(std::to_wstring(n)) : "invia";
}

void MainPage::Paint(Photo^ p)
{
    auto c = dynamic_cast<ContentControl^>(Photos->ContainerFromItem(p));
    if (c != nullptr) Paint(c, p);
}

void MainPage::Paint(ContentControl^ c, Photo^ p)
{
    auto root = dynamic_cast<Panel^>(c->ContentTemplateRoot);
    if (root == nullptr || root->Children->Size < 3) return;
    auto img = dynamic_cast<Image^>(root->Children->GetAt(0));
    auto sent = dynamic_cast<Border^>(root->Children->GetAt(1));
    auto prog = dynamic_cast<Border^>(root->Children->GetAt(2));
    img->Source = p->Thumb;
    sent->Visibility = p->Sent != nullptr && p->Progress == nullptr ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    prog->Visibility = p->Progress != nullptr ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    dynamic_cast<TextBlock^>(prog->Child)->Text = p->Progress != nullptr ? p->Progress : "";
}

void MainPage::OnContainerContentChanging(ListViewBase^, ContainerContentChangingEventArgs^ e)
{
    auto p = dynamic_cast<Photo^>(e->Item);
    if (p == nullptr) return;
    if (e->InRecycleQueue) return;
    Paint(e->ItemContainer, p);
    if (p->Thumb != nullptr || p->ThumbAsked) return;
    p->ThumbAsked = true;
    create_task(p->File->GetThumbnailAsync(ThumbnailMode::PicturesView, 160, ThumbnailOptions::ResizeThumbnail))
        .then([this, p](task<StorageItemThumbnail^> t)
    {
        try
        {
            auto th = t.get();
            if (th == nullptr) return;
            auto bmp = ref new BitmapImage();
            bmp->SetSource(th);
            p->Thumb = bmp;
            Paint(p);
        }
        catch (Exception^ ex)
        {
            Log("miniatura " + Narrow(p->Name->Data()) + ": " + Narrow(ex->Message->Data()));
        }
    }, task_continuation_context::use_current());
}

void MainPage::OnSelectionChanged(Object^, SelectionChangedEventArgs^)
{
    UpdateButtons();
}

void MainPage::OnClear(Object^, RoutedEventArgs^)
{
    Photos->SelectedItems->Clear();
}

void MainPage::OnRefresh(Object^, RoutedEventArgs^)
{
    if (busy_ || loading_) return;
    loading_ = true;
    items_->Clear();
    Status->Text = L"aggiorno\u2026";
    ThreadPool::RunAsync(ref new WorkItemHandler([this](IAsyncAction^) { Load(); }));
}

void MainPage::OnSend(Object^, RoutedEventArgs^)
{
    if (busy_ || Photos->SelectedItems->Size == 0) return;
    // oldest first, in Camera Roll order
    auto queue = ref new Vector<Photo^>();
    for (unsigned i = items_->Size; i-- > 0;)
    {
        auto p = dynamic_cast<Photo^>(items_->GetAt(i));
        unsigned idx;
        if (Photos->SelectedItems->IndexOf(p, &idx))
        {
            p->Progress = "in coda";
            Paint(p);
            queue->Append(p);
        }
    }
    busy_ = true;
    UpdateButtons();
    try { display_->RequestActive(); } catch (Exception^) {}
    ThreadPool::RunAsync(ref new WorkItemHandler([this, queue](IAsyncAction^) { Upload(queue); }));
}

// ---- loading ----

void MainPage::Load()
{
    try
    {
        // sent registry: name \t Pixel file
        std::map<std::wstring, std::wstring> sent;
        try
        {
            auto f = create_task(ApplicationData::Current->LocalFolder->GetFileAsync("sent.txt")).get();
            std::wstring text = create_task(FileIO::ReadTextAsync(f)).get()->Data();
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find(L'\n', pos);
                if (nl == std::wstring::npos) nl = text.size();
                std::wstring line = text.substr(pos, nl - pos);
                pos = nl + 1;
                size_t tab = line.find(L'\t');
                if (tab != std::wstring::npos) sent[Lower(line.substr(0, tab))] = line.substr(tab + 1);
            }
        }
        catch (Exception^) {}
        {
            std::lock_guard<std::mutex> lock(mu_);
            sent_ = sent;
        }

        auto files = create_task(KnownFolders::CameraRoll->GetFilesAsync()).get();
        std::map<std::wstring, StorageFile^> high;
        std::vector<Photo^> photos;
        for (auto f : files)
        {
            std::wstring name = f->Name->Data();
            if (!EndsWithI(name, L".jpg") && !EndsWithI(name, L".jpeg")) continue;
            if (EndsWithI(name, L"__highres.jpg"))
            {
                high[Lower(name.substr(0, name.size() - 13))] = f;
                continue;
            }
            auto p = ref new Photo();
            p->Name = f->Name;
            p->File = f;
            p->Created = ToUnixMs(f->DateCreated);
            photos.push_back(p);
        }
        for (auto p : photos)
        {
            std::wstring name = p->Name->Data();
            size_t dot = name.rfind(L'.');
            auto it = high.find(Lower(name.substr(0, dot)));
            if (it != high.end()) p->High = it->second;
            auto s = sent.find(Lower(name));
            if (s != sent.end()) p->Sent = S(s->second);
        }
        std::sort(photos.begin(), photos.end(), [](Photo^ a, Photo^ b) { return a->Created > b->Created; });
        Log("Rullino: " + std::to_string(photos.size()) + " foto, " + std::to_string(high.size()) + " highres");
        Ui([this, photos]()
        {
            items_->Clear();
            for (auto p : photos) items_->Append(p);
            loading_ = false;
            UpdateButtons();
        });
    }
    catch (Exception^ ex)
    {
        Log("Rullino: " + Narrow(ex->Message->Data()));
        SetStatus("Rullino non leggibile: " + ex->Message);
        Ui([this]() { loading_ = false; });
        return;
    }
    if (FindPixel()) SetStatus((viaTs_ ? ref new String(L"Pixel via Tailscale") : "Pixel: " + S(host_))
        + L" \u00B7 tocca le foto da mandare");
    else SetStatus(L"Pixel non trovato (Wi-Fi e Tailscale): \"aggiorna\" per riprovare; stato nel men\u00F9 \u2026");
}

// ---- finding the Pixel (its IP changes when the Wi-Fi reconnects) ----

bool MainPage::Ping(const std::wstring& host)
{
    try
    {
        auto req = ref new HttpRequestMessage(HttpMethod::Get,
            ref new Uri(S(L"http://" + host + L":" + std::to_wstring(PORT) + L"/ping")));
        cancellation_token_source cts;
        auto timer = ThreadPoolTimer::CreateTimer(ref new TimerElapsedHandler([cts](ThreadPoolTimer^) { cts.cancel(); }),
            TimeSpan{ 2500 * 10000LL });
        auto r = create_task(client_->SendRequestAsync(req), cts.get_token()).get();
        timer->Cancel();
        if (r->StatusCode != HttpStatusCode::Ok) return false;
        std::wstring body = create_task(r->Content->ReadAsStringAsync()).get()->Data();
        return body.find(L"pixelfoto") != std::wstring::npos;
    }
    catch (...)
    {
        return false;
    }
}

std::wstring MainPage::Broadcast()
{
    struct Found { std::mutex m; std::condition_variable cv; std::wstring host; };
    auto found = std::make_shared<Found>();
    try
    {
        auto sock = ref new DatagramSocket();
        sock->MessageReceived += ref new TypedEventHandler<DatagramSocket^, DatagramSocketMessageReceivedEventArgs^>(
            [found](DatagramSocket^, DatagramSocketMessageReceivedEventArgs^ e)
        {
            try
            {
                auto r = e->GetDataReader();
                std::wstring s = r->ReadString(r->UnconsumedBufferLength)->Data();
                if (s.compare(0, 10, L"FOTOPIXEL ") != 0) return;
                std::lock_guard<std::mutex> lock(found->m);
                found->host = e->RemoteAddress->CanonicalName->Data();
                found->cv.notify_all();
            }
            catch (...) {}
        });
        create_task(sock->BindServiceNameAsync("")).get();
        auto out = create_task(sock->GetOutputStreamAsync(ref new HostName("255.255.255.255"),
            S(std::to_wstring(UDP_PORT)))).get();
        auto w = ref new DataWriter(out);
        for (int i = 0; i < 3; i++)
        {
            w->WriteString("FOTOPIXEL?");
            create_task(w->StoreAsync()).get();
            std::unique_lock<std::mutex> lock(found->m);
            if (found->cv.wait_for(lock, std::chrono::milliseconds(1000), [found] { return !found->host.empty(); })) break;
        }
        delete sock;
    }
    catch (Exception^ ex)
    {
        Log("broadcast: " + Narrow(ex->Message->Data()));
    }
    std::lock_guard<std::mutex> lock(found->m);
    return found->host;
}

bool MainPage::FindPixel()
{
    std::wstring last;
    {
        std::lock_guard<std::mutex> lock(mu_);
        last = host_;
    }
    std::wstring found;
    bool wifi = false;
    try
    {
        auto prof = NetworkInformation::GetInternetConnectionProfile();
        wifi = prof != nullptr && prof->IsWlanConnectionProfile;
    }
    catch (Exception^) {}
    if (!wifi)
    {
        // the IP of home is useless here, but a Wi-Fi without internet (no profile) still deserves a quick try
        if (Ping(last)) found = last;
    }
    else if (Ping(last)) found = last;
    if (found.empty() && wifi)
    {
        SetStatus(L"cerco il Pixel in Wi-Fi\u2026");
        std::wstring b = Broadcast();
        if (!b.empty() && Ping(b)) found = b;
    }
    if (found.empty() && wifi)
    {
        // Last resort, if the router drops broadcasts: /ping on every address of our /24, 24 at a time.
        std::wstring prefix;
        for (auto h : NetworkInformation::GetHostNames())
        {
            if (h->Type != HostNameType::Ipv4) continue;
            std::wstring ip = h->CanonicalName->Data();
            size_t dot = ip.rfind(L'.');
            if (dot != std::wstring::npos && ip.compare(0, 4, L"127.") != 0) { prefix = ip.substr(0, dot + 1); break; }
        }
        if (!prefix.empty())
        {
            SetStatus("cerco il Pixel su " + S(prefix) + L"x\u2026");
            Log("scansione " + Narrow(prefix) + "x");
            std::mutex m;
            for (int base = 1; base < 255 && found.empty(); base += 24)
            {
                std::vector<task<void>> batch;
                for (int i = base; i < base + 24 && i < 255; i++)
                {
                    std::wstring ip = prefix + std::to_wstring(i);
                    batch.push_back(create_task([this, ip, &m, &found]()
                    {
                        if (Ping(ip)) { std::lock_guard<std::mutex> lock(m); if (found.empty()) found = ip; }
                    }));
                }
                when_all(batch.begin(), batch.end()).wait();
            }
        }
    }
    if (found.empty())
    {
        SetStatus(L"cerco il Pixel via Tailscale\u2026");
        if (TsPing())
        {
            std::lock_guard<std::mutex> lock(mu_);
            viaTs_ = true;
            Log("Pixel via Tailscale");
            return true;
        }
        Log("Pixel non trovato");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        host_ = found;
        viaTs_ = false;
    }
    ApplicationData::Current->LocalSettings->Values->Insert(L"host", S(found));
    Log("Pixel su " + Narrow(found));
    return true;
}

// ---- HTTP ----

MainPage::Resp MainPage::Call(HttpMethod^ method, String^ path, IBuffer^ body, bool json, int timeoutMs)
{
    std::wstring host;
    bool ts;
    {
        std::lock_guard<std::mutex> lock(mu_);
        host = host_;
        ts = viaTs_;
    }
    if (ts) return TsCall(Narrow(method->Method->Data()), Narrow(path->Data()), body, json, timeoutMs);
    IHttpContent^ content = nullptr;
    if (body != nullptr)
    {
        auto bc = ref new HttpBufferContent(body);
        if (json) bc->Headers->ContentType = ref new Headers::HttpMediaTypeHeaderValue("application/json");
        content = bc;
    }
    try
    {
        auto req = ref new HttpRequestMessage(method,
            ref new Uri(S(L"http://" + host + L":" + std::to_wstring(PORT)) + path));
        req->Headers->Append("X-Token", ref new String(PIXEL_TOKEN));
        if (content != nullptr) req->Content = content;
        cancellation_token_source cts;
        auto timer = ThreadPoolTimer::CreateTimer(ref new TimerElapsedHandler([cts](ThreadPoolTimer^) { cts.cancel(); }),
            TimeSpan{ timeoutMs * 10000LL });
        auto r = create_task(client_->SendRequestAsync(req), cts.get_token()).get();
        auto body = create_task(r->Content->ReadAsStringAsync()).get();
        timer->Cancel();
        Resp out;
        out.status = (int)r->StatusCode;
        out.json = nullptr;
        JsonObject^ j;
        if (JsonObject::TryParse(body, &j)) out.json = j;
        return out;
    }
    catch (task_canceled&)
    {
        throw NetError{ L"il Pixel non risponde" };
    }
    catch (Exception^ ex)
    {
        throw NetError{ ex->Message != nullptr ? std::wstring(ex->Message->Data()) : L"errore di rete" };
    }
}

// ---- upload ----

void MainPage::Upload(IVector<Photo^>^ queue)
{
    int ok = 0, failed = 0, total = (int)queue->Size;
    std::wstring lastError;
    for (int i = 0; i < total; i++)
    {
        auto p = queue->GetAt(i);
        String^ file = nullptr;
        // network errors: find the Pixel again (new IP after a Wi-Fi reconnect) and resume where it stopped
        for (int attempt = 0; attempt < 4 && file == nullptr; attempt++)
        {
            try
            {
                file = SendOne(p, i, total);
            }
            catch (NetError& e)
            {
                lastError = e.what;
                Log("invio " + Narrow(p->Name->Data()) + " tentativo " + std::to_string(attempt + 1) + ": " + Narrow(e.what));
                SetStatus("rete: " + S(e.what) + L" \u00B7 ricerco il Pixel\u2026");
                std::this_thread::sleep_for(std::chrono::milliseconds(1500 * (attempt + 1)));
                FindPixel();
            }
            catch (Exception^ ex)
            {
                lastError = ex->Message->Data();
                Log("invio " + Narrow(p->Name->Data()) + ": " + Narrow(lastError));
                break;
            }
        }
        if (file != nullptr)
        {
            ok++;
            std::wstring line = std::wstring(p->Name->Data()) + L"\t" + file->Data() + L"\n";
            {
                std::lock_guard<std::mutex> lock(mu_);
                sent_[Lower(p->Name->Data())] = file->Data();
            }
            try
            {
                auto f = create_task(ApplicationData::Current->LocalFolder->CreateFileAsync("sent.txt",
                    CreationCollisionOption::OpenIfExists)).get();
                create_task(FileIO::AppendTextAsync(f, S(line))).get();
            }
            catch (Exception^) {}
            Ui([this, p, file]()
            {
                p->Sent = file;
                p->Progress = nullptr;
                Paint(p);
                unsigned idx;
                if (Photos->SelectedItems->IndexOf(p, &idx)) Photos->SelectedItems->RemoveAt(idx);
            });
        }
        else
        {
            failed++;
            Ui([this, p]() { p->Progress = "non inviata"; Paint(p); });
        }
    }
    String^ msg;
    if (failed == 0) msg = (ok == 1 ? "1 foto inviata" : S(std::to_wstring(ok)) + " foto inviate") + (viaTs_ ? ref new String(L" \u00B7 via Tailscale") : L" \u00B7 Pixel " + S(host_));
    else msg = S(std::to_wstring(ok)) + " inviate, " + S(std::to_wstring(failed)) + " no (" + S(lastError)
        + "): restano selezionate, premi invia per riprovare";
    Log(Narrow(msg->Data()));
    Ui([this, msg]()
    {
        busy_ = false;
        Status->Text = msg;
        try { display_->RequestRelease(); } catch (Exception^) {}
        UpdateButtons();
    });
}

String^ MainPage::SendOne(Photo^ p, int index, int total)
{
    StorageFile^ src = p->High != nullptr ? p->High : p->File;
    String^ head = (total > 1 ? S(std::to_wstring(index + 1)) + "/" + S(std::to_wstring(total)) + " " : "") + p->Name;
    auto progress = [this, p, head](const wchar_t* what, unsigned long long done, unsigned long long size)
    {
        int pct = size > 0 ? (int)(done * 100 / size) : 0;
        String^ over = ref new String(what) + " " + S(std::to_wstring(pct)) + "%";
        String^ st = head + ": " + over + " di " + Mb(size);
        Ui([this, p, over, st]() { p->Progress = over; Paint(p); Status->Text = st; });
    };

    auto stream = create_task(src->OpenAsync(FileAccessMode::Read)).get();
    unsigned long long size = stream->Size;

    // SHA-256 of the whole file: the Pixel uses it as the id, to resume and to skip what it already has
    auto hash = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256)->CreateHash();
    unsigned long long done = 0;
    while (done < size)
    {
        auto buf = ref new Buffer(CHUNK);
        IBuffer^ b = create_task(stream->ReadAsync(buf, CHUNK, InputStreamOptions::None)).get();
        if (b->Length == 0) break;
        hash->Append(b);
        done += b->Length;
        progress(L"preparo", done, size);
    }
    String^ sha = CryptographicBuffer::EncodeToHexString(hash->GetValueAndReset());

    auto begin = ref new JsonObject();
    begin->SetNamedValue("name", JsonValue::CreateStringValue(src->Name));
    begin->SetNamedValue("size", JsonValue::CreateNumberValue((double)size));
    begin->SetNamedValue("sha256", JsonValue::CreateStringValue(sha));
    begin->SetNamedValue("taken", JsonValue::CreateNumberValue((double)p->Created));
    Resp r = Call(HttpMethod::Post, "/begin", CryptographicBuffer::ConvertStringToBinary(begin->Stringify(),
        BinaryStringEncoding::Utf8), true, 15000);
    if (r.status == 403) throw ref new FailureException("chiave rifiutata dal Pixel");
    if (r.status != 200 || r.json == nullptr) throw NetError{ L"begin: HTTP " + std::to_wstring(r.status) };
    if (r.json->GetNamedString("status") == "done")
    {
        Log(Narrow(p->Name->Data()) + ": gia' sul Pixel");
        return r.json->GetNamedString("file");
    }
    unsigned long long offset = (unsigned long long)r.json->GetNamedNumber("offset");
    if (offset > 0) Log(Narrow(p->Name->Data()) + ": riprendo da " + std::to_string(offset));

    ULONGLONG t0 = GetTickCount64();
    while (offset < size)
    {
        progress(L"invio", offset, size);
        stream->Seek(offset);
        unsigned chunk = viaTs_ ? CHUNK / 4 : CHUNK;   // over DERP: smaller pieces, progress and resume more often
        auto buf = ref new Buffer(chunk);
        IBuffer^ b = create_task(stream->ReadAsync(buf, chunk, InputStreamOptions::None)).get();
        if (b->Length == 0) throw ref new FailureException("file letto solo in parte");
        r = Call(HttpMethod::Put, "/chunk?id=" + sha + "&offset=" + S(std::to_wstring(offset)),
            b, false, 60000);
        if (r.status == 200 || r.status == 409)
        {
            if (r.json == nullptr) throw NetError{ L"risposta vuota" };
            offset = (unsigned long long)r.json->GetNamedNumber("offset");
        }
        else if (r.status == 400 && r.json != nullptr)
        {
            // the Pixel lost the upload (cleanup after 3 days, reinstall): start it again
            Log("chunk 400: " + Narrow(r.json->GetNamedString("error", "")->Data()));
            r = Call(HttpMethod::Post, "/begin", CryptographicBuffer::ConvertStringToBinary(begin->Stringify(),
                BinaryStringEncoding::Utf8), true, 15000);
            if (r.status != 200 || r.json == nullptr) throw NetError{ L"begin: HTTP " + std::to_wstring(r.status) };
            if (r.json->GetNamedString("status") == "done") return r.json->GetNamedString("file");
            offset = (unsigned long long)r.json->GetNamedNumber("offset");
        }
        else throw NetError{ L"chunk: HTTP " + std::to_wstring(r.status) };
    }
    progress(L"salvo", size, size);
    r = Call(HttpMethod::Post, "/end?id=" + sha, nullptr, false, 120000);
    if (r.json == nullptr) throw NetError{ L"end: HTTP " + std::to_wstring(r.status) };
    String^ status = r.json->GetNamedString("status", "");
    if (status == "saved" || status == "done")
    {
        double secs = (GetTickCount64() - t0) / 1000.0;
        char b[96];
        sprintf_s(b, " %.1f MB in %.1f s", size / 1048576.0, secs);
        Log(Narrow(p->Name->Data()) + " (" + Narrow(src->Name->Data()) + ") -> " +
            Narrow(r.json->GetNamedString("file")->Data()) + b);
        return r.json->GetNamedString("file");
    }
    if (status == "upload") throw NetError{ L"file incompleto sul Pixel" };   // retry resumes from its offset
    throw ref new FailureException(r.json->GetNamedString("error", "errore del Pixel"));
}
