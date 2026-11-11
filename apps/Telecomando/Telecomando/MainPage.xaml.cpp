#include "pch.h"
#include "MainPage.xaml.h"
#include "Core/TvRemote.h"
#include "tsio.h"

#include <cstring>
#include <mutex>
#include <cstdio>

using namespace Telecomando;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    const wchar_t* DefaultHost = L"192.168.1.250";
    const wchar_t* DefaultMac = L"00:11:22:33:44:55";

    std::string Narrow(String^ s)
    {
        std::string out;
        if (s == nullptr) return out;
        for (const wchar_t* p = s->Data(); *p; ++p) out += static_cast<char>(*p < 128 ? *p : '?');
        return out;
    }

    String^ Wide(const std::string& s)
    {
        // UTF-8 -> UTF-16 (the states are Italian text)
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
        return ref new String(w.c_str());
    }

    String^ Setting(String^ key, String^ fallback)
    {
        auto v = ApplicationData::Current->LocalSettings->Values->Lookup(key);
        String^ s = dynamic_cast<String^>(v);
        return s == nullptr || s->IsEmpty() ? fallback : s;
    }

    TextBlock^ Glyph(Symbol s, double size)
    {
        auto t = ref new TextBlock();
        wchar_t c[2] = { static_cast<wchar_t>(static_cast<int>(s)), 0 };
        t->Text = ref new String(c);
        t->FontFamily = ref new FontFamily(L"Segoe UI Symbol");
        t->FontSize = size;
        return t;
    }
}

// ------------------------------------------------------------------ log

namespace
{
    std::mutex logMu;
}

void Telecomando::Log(const std::string& line)
{
    std::lock_guard<std::mutex> l(logMu);
    try
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        sprintf_s(stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\run.log";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f != nullptr)
        {
            // keep it small: start again past 256 KB
            if (_ftelli64(f) > 256 * 1024) { fclose(f); _wfopen_s(&f, path.c_str(), L"wb"); }
            if (f != nullptr)
            {
                fprintf(f, "%s%s\r\n", stamp, line.c_str());
                fclose(f);
            }
        }
        OutputDebugStringA((stamp + line + "\n").c_str());
    }
    catch (...) {}
}

// ------------------------------------------------------------------ Link

Link::Link(StateFn onState) : onState_(onState), active_(false), stop_(false), reconnect_(false), pause_(false), retryAt_(0),
    shownConnected_(false), wakeUntil_(0), remote_(new tv::Remote())
{
    thread_ = std::thread([this]
    {
        try { Run(); }
        catch (Exception^ e) { Log("link thread: " + std::string(e->Message->Data(), e->Message->Data() + e->Message->Length())); }
        catch (const std::exception& e) { Log(std::string("link thread: ") + e.what()); }
        catch (...) { Log("link thread: unknown exception"); }
    });
}

Link::~Link()
{
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    static_cast<tv::Remote*>(remote_)->close();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    delete static_cast<tv::Remote*>(remote_);
}

void Link::Configure(const std::string& host, const std::string& token)
{
    std::lock_guard<std::mutex> l(mu_);
    if (host != host_) reconnect_ = true;
    host_ = host;
    token_ = token;
    cv_.notify_all();
}

void Link::Key(const std::string& key)
{
    std::lock_guard<std::mutex> l(mu_);
    // a held key repeats: never more than two of the same waiting (no flood after a slow moment)
    int same = 0;
    for (size_t i = 0; i < keys_.size(); ++i) if (keys_[i] == key) ++same;
    if (same < 2 && keys_.size() < 10) keys_.push_back(key);
    retryAt_ = 0;   // a key pressed while disconnected: try now
    cv_.notify_all();
}

// The packet goes out at once on its own thread: the worker may be inside a connect that waits seconds.
void Link::Wake(const std::string& mac)
{
    std::string host;
    {
        std::lock_guard<std::mutex> l(mu_);
        host = host_;
        wakeUntil_ = ts::nowMs() + 40000;
        retryAt_ = 0;
        cv_.notify_all();
    }
    // only a status for a TV that is not linked: "collegato" must stay true, or power would wake forever
    bool linked;
    { std::lock_guard<std::mutex> l(mu_); linked = shownConnected_; }
    if (!linked) State("accendo la TV...", false);
    std::thread([mac, host]
    {
        try
        {
            // twice: the first packet can be lost while the TV network chip is half asleep
            for (int i = 0; i < 2; ++i)
            {
                std::string e = tv::wakeOnLan(mac, host);
                Log("wake-on-lan " + mac + ": " + (e.empty() ? std::string("sent") : e));
                if (!e.empty()) break;
                if (i == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }
        }
        catch (...) { Log("wake-on-lan: exception"); }
    }).detach();
}

void Link::State(const std::string& state, bool connected)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        if (state == shown_ && connected == shownConnected_) return;
        shown_ = state;
        shownConnected_ = connected;
    }
    onState_(state, connected);
}

void Link::Start()
{
    std::lock_guard<std::mutex> l(mu_);
    active_ = true;
    retryAt_ = 0;
    cv_.notify_all();
}

void Link::Pause()
{
    std::lock_guard<std::mutex> l(mu_);
    active_ = false;
    pause_ = true;    // the worker closes the connection (it may be reading it right now)
    keys_.clear();
    cv_.notify_all();
}

void Link::Reconnect()
{
    std::lock_guard<std::mutex> l(mu_);
    reconnect_ = true;
    retryAt_ = 0;
    cv_.notify_all();
}

std::string Link::Token()
{
    std::lock_guard<std::mutex> l(mu_);
    return token_;
}

void Link::Run()
{
    tv::Remote& r = *static_cast<tv::Remote*>(remote_);
    r.log = [](const std::string& s) { Log(s); };
    bool wasConnected = false;
    bool tvOff = false;
    while (true)
    {
        std::string key, host, token;
        int64_t wakeUntil = 0;
        bool doReconnect = false;
        {
            std::unique_lock<std::mutex> l(mu_);
            if (stop_) return;
            if (pause_)
            {
                pause_ = false;
                l.unlock();
                if (r.connected()) Log("paused: closing the connection");
                r.close();
                wasConnected = false;
                tvOff = false;
                { std::lock_guard<std::mutex> l2(mu_); shown_.clear(); }
                continue;
            }
            if (!active_) { cv_.wait(l); continue; }
            if (!keys_.empty()) { key = keys_.front(); keys_.pop_front(); }
            host = host_;
            token = token_;
            doReconnect = reconnect_;
            reconnect_ = false;
            wakeUntil = wakeUntil_;
        }
        if (doReconnect) r.close();
        if (!r.connected())
        {
            if (wasConnected) { wasConnected = false; State("collegamento perso, ricollego...", false); }
            int64_t now = ts::nowMs();
            int64_t retryAt;
            {
                std::lock_guard<std::mutex> l(mu_);
                retryAt = retryAt_;
            }
            if (now < retryAt)
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait_for(l, std::chrono::milliseconds(retryAt - now));
                if (!key.empty()) keys_.push_front(key);
                continue;
            }
            // A TV in standby does not answer at all: a short plain TCP probe first, so an off TV is
            // a quiet "spenta" (retried every few seconds, every second while it boots after a wake)
            // and not an 8 s connect that fails over and over on screen.
            bool waking = now < wakeUntil;
            {
                std::string perr;
                std::unique_ptr<ts::TcpStream> probe = ts::TcpStream::connect(host, 8002, 1500, perr);
                if (!probe)
                {
                    if (!tvOff) Log("tv off (no answer on " + host + ":8002)");
                    tvOff = true;
                    {
                        std::lock_guard<std::mutex> l(mu_);
                        if (retryAt_ == 0 || retryAt_ <= ts::nowMs()) retryAt_ = ts::nowMs() + (waking ? 1000 : 4000);
                    }
                    State(waking ? "accendo la TV..." : "TV spenta", false);
                    if (!key.empty()) { std::lock_guard<std::mutex> l(mu_); keys_.push_front(key); }
                    continue;
                }
                probe->close();
            }
            if (tvOff) Log("tv answers");
            tvOff = false;
            State(token.empty() ? "collego... sulla TV scegli Consenti" : "collego...", false);
            Log("connect to " + host + (token.empty() ? " (no token: the TV asks)" : " with token"));
            std::string before = token;
            std::string err = r.connect(host, "Lumia", token, token.empty() ? 30000 : 8000);
            if (!err.empty())
            {
                {
                    std::lock_guard<std::mutex> l(mu_);
                    retryAt_ = ts::nowMs() + (waking ? 1000 : 4000);
                }
                // a TV that is still booting answers TCP before its remote service: not an error yet
                State(waking ? "accendo la TV..." : err, false);
                Log("connect failed: " + err);
                if (!key.empty()) { std::lock_guard<std::mutex> l(mu_); keys_.push_front(key); }
                continue;
            }
            Log("step: connect returned");
            if (token != before) { std::lock_guard<std::mutex> l(mu_); token_ = token; }
            wasConnected = true;
            Log("connected");
            {
                std::lock_guard<std::mutex> l(mu_);
                wakeUntil_ = 0;
            }
            State("collegato", true);
        }
        if (!key.empty())
        {
            if (!r.sendKey(key))
            {
                Log("send " + key + " failed");
                std::lock_guard<std::mutex> l(mu_);
                keys_.push_front(key);
            }
            continue;
        }
        // idle: read the TV (pings) until a key comes
        r.poll(150);
    }
}

// ------------------------------------------------------------------ page

MainPage::MainPage() : link_(nullptr), playing_(true), lastBuzz_(0), connected_(false)
{
    Log("start");
    InitializeComponent();
    HomeButton->Content = Glyph(Symbol::Home, 24);
    BackButton->Content = Glyph(Symbol::Back, 24);
    PlayButton->Content = Glyph(Symbol::Play, 22);
    MenuButton->Content = Glyph(Symbol::Setting, 22);
    MuteButton->Content = Glyph(Symbol::Mute, 22);
    UpButton->Content = Glyph(Symbol::Up, 26);
    auto down = Glyph(Symbol::Up, 26);
    down->RenderTransformOrigin = Point(0.5, 0.5);
    auto flip = ref new RotateTransform();
    flip->Angle = 180;
    down->RenderTransform = flip;
    DownButton->Content = down;
    LeftButton->Content = Glyph(Symbol::Back, 26);
    RightButton->Content = Glyph(Symbol::Forward, 26);
    NumbersCommand->Icon = ref new SymbolIcon(Symbol::Keyboard);
    SettingsCommand->Icon = ref new SymbolIcon(Symbol::Setting);

    // 1 2 3 / 4 5 6 / 7 8 9 / - 0 OK
    for (int i = 0; i < 4; ++i)
    {
        auto row = ref new RowDefinition();
        NumberGrid->RowDefinitions->Append(row);
    }
    for (int i = 0; i < 3; ++i) NumberGrid->ColumnDefinitions->Append(ref new ColumnDefinition());
    const wchar_t* labels[] = { L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"\u2013", L"0", L"OK" };
    const char* keys[] = { "KEY_1", "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_6", "KEY_7", "KEY_8", "KEY_9", "KEY_PLUS100", "KEY_0", "KEY_ENTER" };
    for (int i = 0; i < 12; ++i)
    {
        auto b = ref new Button();
        b->Style = safe_cast<Windows::UI::Xaml::Style^>(Application::Current->Resources->Lookup(L"RemoteButton"));
        b->Content = ref new String(labels[i]);
        b->FontSize = 28;
        b->Margin = Thickness(6);
        b->Tag = ref new String(std::wstring(keys[i], keys[i] + strlen(keys[i])).c_str());
        b->Click += ref new RoutedEventHandler(this, &MainPage::Key_Click);
        Grid::SetRow(b, i / 3);
        Grid::SetColumn(b, i % 3);
        NumberGrid->Children->Append(b);
    }

    auto dispatcher = Dispatcher;
    link_ = new Link([this, dispatcher](const std::string& state, bool connected)
    {
        dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, ref new Windows::UI::Core::DispatchedHandler(
            [this, state, connected]() { ShowState(state, connected); }));
    });
}

void MainPage::OnNavigatedTo(NavigationEventArgs^)
{
    TvName->Text = Setting(L"name", L"Tele");
    link_->Configure(Narrow(Setting(L"host", ref new String(DefaultHost))), Narrow(Setting(L"token", L"")));
    link_->Start();
}

void MainPage::Suspend() { Log("suspend"); StopBuzz(); link_->Pause(); }
void MainPage::Resume() { Log("resume"); link_->Start(); }

bool MainPage::HandleBack()
{
    if (Numbers->Visibility == Windows::UI::Xaml::Visibility::Visible)
    {
        Numbers->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        return true;
    }
    return false;
}

void MainPage::ShowState(const std::string& state, bool connected)
{
    Log("ui: " + state);
    connected_ = connected;
    Status->Text = Wide(state);
    Status->Foreground = connected ? safe_cast<Brush^>(Application::Current->Resources->Lookup(L"Accent"))
                                   : safe_cast<Brush^>(Application::Current->Resources->Lookup(L"Dim"));
    if (connected)
    {
        std::string token = link_->Token();
        if (!token.empty()) ApplicationData::Current->LocalSettings->Values->Insert(L"token", Wide(token));
    }
}

void MainPage::Buzz()
{
    // once per press: a held key repeats every 130 ms and must not buzz all the time
    ULONGLONG now = GetTickCount64();
    bool repeat = now - lastBuzz_ < 400;
    lastBuzz_ = now;
    if (repeat) return;
    try
    {
        // A 15 ms Vibrate could leave the Lumia motor running on its own: a 30 ms one and an explicit Cancel
        // after 80 ms, plus on suspend
        TimeSpan t;
        t.Duration = 300000;   // 30 ms
        Windows::Phone::Devices::Notification::VibrationDevice::GetDefault()->Vibrate(t);
        Log("buzz");
        if (buzzStop_ == nullptr)
        {
            buzzStop_ = ref new DispatcherTimer();
            TimeSpan d;
            d.Duration = 800000;   // 80 ms
            buzzStop_->Interval = d;
            buzzStop_->Tick += ref new Windows::Foundation::EventHandler<Object^>([this](Object^, Object^) { StopBuzz(); });
        }
        buzzStop_->Stop();
        buzzStop_->Start();
    }
    catch (Exception^ e) { Log("buzz failed: " + Narrow(e->Message)); }
}

void MainPage::StopBuzz()
{
    if (buzzStop_ != nullptr) buzzStop_->Stop();
    try { Windows::Phone::Devices::Notification::VibrationDevice::GetDefault()->Cancel(); }
    catch (Exception^ e) { Log("buzz cancel failed: " + Narrow(e->Message)); }
}

void MainPage::Send(const std::string& key)
{
    Buzz();
    link_->Key(key);
}

void MainPage::Key_Click(Object^ sender, RoutedEventArgs^)
{
    auto el = dynamic_cast<FrameworkElement^>(sender);
    if (el == nullptr) return;
    String^ tag = dynamic_cast<String^>(el->Tag);
    if (tag != nullptr) Send(Narrow(tag));
}

// On: the TV in standby does not answer, so a Wake-on-LAN packet first; off: its power key.
void MainPage::Power_Click(Object^, RoutedEventArgs^)
{
    Buzz();
    if (connected_) link_->Key("KEY_POWER");
    else link_->Wake(Narrow(Setting(L"mac", ref new String(DefaultMac))));
}

void MainPage::Play_Click(Object^, RoutedEventArgs^)
{
    Send(playing_ ? "KEY_PAUSE" : "KEY_PLAY");
    playing_ = !playing_;
    PlayButton->Content = Glyph(playing_ ? Symbol::Play : Symbol::Pause, 22);
}

void MainPage::Numbers_Click(Object^, RoutedEventArgs^)
{
    Numbers->Visibility = Numbers->Visibility == Windows::UI::Xaml::Visibility::Visible
        ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::Wake_Click(Object^, RoutedEventArgs^)
{
    link_->Wake(Narrow(Setting(L"mac", ref new String(DefaultMac))));
}

void MainPage::Reconnect_Click(Object^, RoutedEventArgs^)
{
    link_->Reconnect();
}

void MainPage::Settings_Click(Object^, RoutedEventArgs^)
{
    auto panel = ref new StackPanel();
    auto addBox = [panel](String^ header, String^ value)
    {
        auto box = ref new TextBox();
        box->Header = header;
        box->Text = value;
        box->Margin = Thickness(0, 0, 0, 8);
        panel->Children->Append(box);
        return box;
    };
    TextBox^ name = addBox(L"Nome", Setting(L"name", L"Tele"));
    TextBox^ host = addBox(L"Indirizzo IP della TV", Setting(L"host", ref new String(DefaultHost)));
    TextBox^ mac = addBox(L"MAC Wi-Fi (per accenderla)", Setting(L"mac", ref new String(DefaultMac)));
    auto note = ref new TextBlock();
    note->Text = L"Per accenderla dal telefono, sulla TV: Impostazioni \u203a Generali \u203a Rete \u203a "
                 L"Impostazioni esperto \u203a Accendi con dispositivo mobile.";
    note->TextWrapping = TextWrapping::Wrap;
    note->FontSize = 14;
    panel->Children->Append(note);
    auto dialog = ref new ContentDialog();
    dialog->Title = L"impostazioni";
    dialog->Content = panel;
    dialog->PrimaryButtonText = L"salva";
    dialog->SecondaryButtonText = L"annulla";
    concurrency::create_task(dialog->ShowAsync()).then([this, name, host, mac](ContentDialogResult r)
    {
        if (r != ContentDialogResult::Primary) return;
        auto values = ApplicationData::Current->LocalSettings->Values;
        bool newTv = !host->Text->Equals(Setting(L"host", ref new String(DefaultHost)));
        values->Insert(L"name", name->Text);
        values->Insert(L"host", host->Text);
        values->Insert(L"mac", mac->Text);
        if (newTv) values->Insert(L"token", L"");   // another TV: ask it again
        TvName->Text = name->Text;
        link_->Configure(Narrow(host->Text), Narrow(Setting(L"token", L"")));
    }, concurrency::task_continuation_context::use_current());
}
