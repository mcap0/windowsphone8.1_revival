#include "pch.h"
#include "TgService.h"
#include "ApiKeys.h"

#include <chrono>
#include <fstream>
#include <sstream>

using namespace Telegram;
using namespace Platform;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Notifications;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::Security::Cryptography;

namespace Telegram
{
    ref class Conn sealed
    {
    internal:
        StreamSocket^ socket;
        DataWriter^ writer;
        DataReader^ reader;
        std::vector<uint8_t> pending;
        bool storing;
        bool closed;
        Conn() : storing(false), closed(false) {}
    };

    std::wstring Utf16(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
        return w;
    }

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    String^ PStr(const std::string& s) { return ref new String(Utf16(s).c_str()); }
    std::string FromP(String^ s) { return s == nullptr ? std::string() : Utf8(std::wstring(s->Data(), s->Length())); }

    void DecodeImage(const std::string& bytes, std::function<void(Windows::UI::Xaml::Media::Imaging::BitmapImage^)> done)
    {
        // WebP (stickers) is not supported by WP8.1: a failed SetSourceAsync left unobserved closes the app
        if (bytes.size() < 12 || (bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0))
        {
            done(nullptr);
            return;
        }
        auto stream = ref new InMemoryRandomAccessStream();
        auto writer = ref new DataWriter(stream);
        writer->WriteBytes(ArrayReference<unsigned char>(reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.data())),
            static_cast<unsigned int>(bytes.size())));
        auto bitmap = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage();
        create_task(writer->StoreAsync()).then([writer, stream, bitmap](unsigned int)
        {
            writer->DetachStream();
            stream->Seek(0);
            return create_task(bitmap->SetSourceAsync(stream));
        }, task_continuation_context::use_current()).then([bitmap, done](task<void> t)
        {
            try { t.get(); }
            catch (Exception^) { done(nullptr); return; }
            catch (...) { done(nullptr); return; }
            done(bitmap);
        }, task_continuation_context::use_current());
    }
}

namespace
{
    std::wstring LocalPath(const wchar_t* name)
    {
        return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + name;
    }

    bool ReadFile(const std::wstring& path, std::string& out)
    {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (!f) return false;
        std::stringstream b;
        b << f.rdbuf();
        out = b.str();
        return true;
    }

    void WriteFile(const std::wstring& path, const std::string& data)
    {
        std::wstring tmp = path + L".tmp";
        {
            std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
            f.write(data.data(), data.size());
        }
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    // tg.seen ("key topId" lines) = newest message of each chat already shown by the app: the background task
    // (TelegramTask) toasts only messages that arrive after the app went away.
    void SaveSeen(const std::vector<tg::ChatItem>& chats)
    {
        std::map<std::string, int32_t> seen;
        std::string text;
        if (ReadFile(LocalPath(L"tg.seen"), text))
        {
            std::istringstream in(text);
            std::string key;
            long long id = 0;
            while (in >> key >> id) seen[key] = static_cast<int32_t>(id);
        }
        for (size_t i = 0; i < chats.size(); ++i)
            if (chats[i].topId > seen[chats[i].key]) seen[chats[i].key] = chats[i].topId;
        std::ostringstream out;
        for (std::map<std::string, int32_t>::const_iterator it = seen.begin(); it != seen.end(); ++it)
            out << it->first << " " << it->second << "\n";
        WriteFile(LocalPath(L"tg.seen"), out.str());
    }

    std::map<std::string, std::vector<std::function<void(const std::string&, bool)> > > g_downloads;
    std::string g_unsent;
}

TgService& TgService::Get()
{
    static TgService* s = new TgService();
    return *s;
}

TgService::TgService()
    : online(false), engine_(nullptr), client_(nullptr), saveDue_(false), saveTicks_(0), chatsDirty_(false),
      logTicks_(0), logSent_(0), fgTicks_(ForegroundTicks)
{
}

void TgService::Init(Windows::UI::Core::CoreDispatcher^ dispatcher)
{
    if (engine_) return;
    dispatcher_ = dispatcher;
    Log("start 3.4.0");
    CreateDirectoryW(LocalPath(L"cache").c_str(), nullptr);
    tg::EngineConfig cfg;
    cfg.apiId = TG_API_ID;
    cfg.deviceModel = "Lumia 1020";
    cfg.systemVersion = "Windows Phone 8.1";
    cfg.appVersion = "3.0";
    cfg.langCode = "it";
    engine_ = new tg::Engine(this, cfg);
    client_ = new tg::Client(engine_, TG_API_ID, TG_API_HASH);
    client_->setListener(this);
    std::string blob;
    if (ReadFile(LocalPath(L"tg.state"), blob)) engine_->load(blob);
    if (ReadFile(LocalPath(L"tg.client"), blob)) client_->load(blob);
    timer_ = ref new DispatcherTimer();
    TimeSpan ts;
    ts.Duration = 5000000;   // 500 ms
    timer_->Interval = ts;
    timer_->Tick += ref new EventHandler<Object^>([this](Object^, Object^) { Tick(); });
    timer_->Start();
    engine_->start();
    client_->start();
}

void TgService::Tick()
{
    if (!engine_) return;
    int64_t t0 = unixTimeMs();
    engine_->tick();
    if (saveDue_ && ++saveTicks_ >= 4) SaveNow();
    int64_t dt = unixTimeMs() - t0;
    if (dt > 150) Log("slow tick " + std::to_string(static_cast<long long>(dt)) + " ms");
    // tg.fg = "the app is on screen" for the background task (TelegramTask), which then skips its toasts
    if (++fgTicks_ >= ForegroundTicks) {
        fgTicks_ = 0;
        WriteFile(LocalPath(L"tg.fg"), std::to_string(static_cast<long long>(unixTimeMs())));
    }
    // diagnostics: ship new log lines to the PC inbox every ~5 s
    if (++logTicks_ >= 10) {
        logTicks_ = 0;
        SendLog();
    }
}

void TgService::SaveSoon()
{
    saveDue_ = true;
    saveTicks_ = 0;
}

void TgService::SaveNow()
{
    saveDue_ = false;
    if (!engine_) return;
    WriteFile(LocalPath(L"tg.state"), engine_->save());
    WriteFile(LocalPath(L"tg.client"), client_->save());
}

void TgService::Suspend()
{
    if (!engine_) return;
    SaveNow();
    SaveSeen(client_->chats());
    engine_->stop();
    timer_->Stop();
    DeleteFileW(LocalPath(L"tg.fg").c_str());
}

void TgService::Resume()
{
    if (!engine_) return;
    fgTicks_ = ForegroundTicks;
    timer_->Start();
    engine_->start();
    client_->onSessionReset();
}

void TgService::AddListener(tg::IClientListener* l)
{
    RemoveListener(l);
    listeners_.push_back(l);
}

void TgService::RemoveListener(tg::IClientListener* l)
{
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}

// ------------------------------------------------------------------ log

void TgService::Log(const std::string& line)
{
    time_t now = time(nullptr);
    tm t;
    localtime_s(&t, &now);
    char b[16];
    sprintf_s(b, "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
    log_.push_back(b + line);
    g_unsent += log_.back() + "\n";
    if (log_.size() > 400) log_.erase(log_.begin(), log_.begin() + 100);
}

std::string TgService::LogText() const
{
    std::string s;
    for (size_t i = 0; i < log_.size(); ++i) s += log_[i] + "\n";
    return s;
}

void TgService::SendLog()
{
    // the inbox appends: send only the lines not sent yet
    std::string text;
    text.swap(g_unsent);
    if (text.empty()) return;
    auto http = ref new Windows::Web::Http::HttpClient();
    auto content = ref new Windows::Web::Http::HttpStringContent(PStr(text));
    create_task(http->PostAsync(ref new Uri(L"http://192.168.1.148:8765/telegram"), content))
        .then([http](task<Windows::Web::Http::HttpResponseMessage^> t) { try { t.get(); } catch (Exception^) {} });
}

// ------------------------------------------------------------------ sockets

void TgService::netConnect(int dc, const std::string& host, int port)
{
    netClose(dc);
    Conn^ c = ref new Conn();
    c->socket = ref new StreamSocket();
    c->socket->Control->NoDelay = true;
    c->socket->Control->KeepAlive = true;
    conns_[dc] = c;
    wchar_t portText[16];
    swprintf_s(portText, L"%d", port);
    create_task(c->socket->ConnectAsync(ref new HostName(PStr(host)), ref new String(portText)))
        .then([this, dc, c](task<void> t)
    {
        try { t.get(); }
        catch (Exception^ ex)
        {
            if (c->closed) return;
            Log("connect failed: " + FromP(ex->Message));
            c->closed = true;
            conns_.erase(dc);
            engine_->onClosed(dc);
            return;
        }
        if (c->closed) return;
        c->writer = ref new DataWriter(c->socket->OutputStream);
        c->reader = ref new DataReader(c->socket->InputStream);
        c->reader->InputStreamOptions = InputStreamOptions::Partial;
        engine_->onConnected(dc);
        Flush(dc, c);
        ReadLoop(dc, c);
    });
}

void TgService::ReadLoop(int dc, Conn^ c)
{
    create_task(c->reader->LoadAsync(65536)).then([this, dc, c](task<unsigned int> t)
    {
        unsigned int n = 0;
        try { n = t.get(); }
        catch (Exception^ ex) { Log("read: " + FromP(ex->Message)); n = 0; }
        if (c->closed) return;
        if (n == 0)
        {
            c->closed = true;
            conns_.erase(dc);
            engine_->onClosed(dc);
            return;
        }
        std::vector<uint8_t> buf(n);
        c->reader->ReadBytes(ArrayReference<unsigned char>(&buf[0], n));
        int64_t t0 = unixTimeMs();
        engine_->onData(dc, &buf[0], n);
        int64_t dt = unixTimeMs() - t0;
        if (dt > 150) Log("slow onData " + std::to_string(static_cast<long long>(dt)) + " ms (" + std::to_string(static_cast<long long>(n)) + " bytes)");
        if (!c->closed) ReadLoop(dc, c);
    });
}

void TgService::netSend(int dc, const std::vector<uint8_t>& data)
{
    auto it = conns_.find(dc);
    if (it == conns_.end() || data.empty()) return;
    Conn^ c = it->second;
    c->pending.insert(c->pending.end(), data.begin(), data.end());
    if (c->writer != nullptr) Flush(dc, c);
}

void TgService::Flush(int dc, Conn^ c)
{
    if (c->closed || c->storing || c->pending.empty() || c->writer == nullptr) return;
    c->storing = true;
    std::vector<uint8_t> out;
    out.swap(c->pending);
    c->writer->WriteBytes(ArrayReference<unsigned char>(&out[0], static_cast<unsigned int>(out.size())));
    create_task(c->writer->StoreAsync()).then([this, dc, c](task<unsigned int> t)
    {
        c->storing = false;
        try { t.get(); }
        catch (Exception^ ex)
        {
            if (c->closed) return;
            Log("write: " + FromP(ex->Message));
            c->closed = true;
            conns_.erase(dc);
            engine_->onClosed(dc);
            return;
        }
        Flush(dc, c);
    });
}

void TgService::netClose(int dc)
{
    auto it = conns_.find(dc);
    if (it == conns_.end()) return;
    Conn^ c = it->second;
    conns_.erase(it);
    c->closed = true;
    try { delete c->socket; } catch (Exception^) {}
}

void TgService::randomBytes(uint8_t* out, std::size_t n)
{
    IBuffer^ b = CryptographicBuffer::GenerateRandom(static_cast<unsigned int>(n));
    Array<unsigned char>^ a;
    CryptographicBuffer::CopyToByteArray(b, &a);
    memcpy(out, a->Data, n);
}

int64_t TgService::unixTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ engine events

void TgService::onUpdates(const tg::TlValue& u) { client_->onUpdates(u); SaveSoon(); }
void TgService::onSessionReset() { client_->onSessionReset(); }
void TgService::onStateChanged() { SaveSoon(); }
void TgService::onAuthLost() { client_->onAuthLost(); SaveSoon(); }

void TgService::onOnline(bool on)
{
    online = on;
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onOnline(on);
}

// ------------------------------------------------------------------ client events

void TgService::onAuthState(int state)
{
    SaveSoon();
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onAuthState(state);
}

void TgService::onChatsChanged()
{
    SaveSoon();
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onChatsChanged();
}

void TgService::onNewMessage(const tg::MsgItem& m)
{
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onNewMessage(m);
    if (m.out || m.chatKey == openChat) return;
    const tg::ChatItem* c = client_->chat(m.chatKey);
    if (c && c->muted) return;
    std::string title = c ? c->title : m.fromName;
    std::string text = tg::Client::previewText(m);
    if (c && c->isGroup) text = m.fromName + ": " + text;
    Toast(title, text, m.chatKey);
}

void TgService::onMessageEdited(const tg::MsgItem& m)
{
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onMessageEdited(m);
}

void TgService::onMessagesDeleted(const std::string& chatKey, const std::vector<int32_t>& ids)
{
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onMessagesDeleted(chatKey, ids);
}

void TgService::onReadOutbox(const std::string& chatKey, int32_t maxId)
{
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onReadOutbox(chatKey, maxId);
}

void TgService::onError(const std::string& what)
{
    Log("error: " + what);
    std::vector<tg::IClientListener*> ls = listeners_;
    for (size_t i = 0; i < ls.size(); ++i) ls[i]->onError(what);
}

void TgService::Toast(const std::string& title, const std::string& text, const std::string& chatKey)
{
    try
    {
        XmlDocument^ xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        XmlNodeList^ texts = xml->GetElementsByTagName(L"text");
        texts->Item(0)->AppendChild(xml->CreateTextNode(PStr(title)));
        texts->Item(1)->AppendChild(xml->CreateTextNode(PStr(text.substr(0, 200))));
        static_cast<XmlElement^>(xml->SelectSingleNode(L"/toast"))->SetAttribute(L"launch", PStr("chat=" + chatKey));
        ToastNotificationManager::CreateToastNotifier()->Show(ref new ToastNotification(xml));
    }
    catch (Exception^) {}
}

// ------------------------------------------------------------------ media

void TgService::GetMedia(const std::string& key, const std::function<void(const std::string&, bool)>& done,
                         const std::function<void(int64_t, int64_t)>& progress)
{
    std::wstring path = LocalPath(L"cache") + L"\\" + Utf16(key);
    std::string data;
    if (ReadFile(path, data) && !data.empty()) { done(data, true); return; }
    std::vector<std::function<void(const std::string&, bool)> >& waiting = g_downloads[key];
    waiting.push_back(done);
    if (waiting.size() > 1) return;
    client_->downloadMedia(key, progress, [this, key, path](const std::string& bytes, const tg::RpcErr& e)
    {
        if (!e.ok()) Log("download " + key + ": " + e.message);
        else WriteFile(path, bytes);
        std::vector<std::function<void(const std::string&, bool)> > cbs;
        cbs.swap(g_downloads[key]);
        g_downloads.erase(key);
        for (size_t i = 0; i < cbs.size(); ++i) cbs[i](bytes, e.ok());
    });
}

void TgService::OpenWithSystem(const std::string& bytes, const std::wstring& name)
{
    auto data = ref new Array<unsigned char>(static_cast<unsigned int>(bytes.size()));
    if (!bytes.empty()) memcpy(data->Data, bytes.data(), bytes.size());
    String^ fileName = ref new String(name.c_str());
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"Downloads", CreationCollisionOption::OpenIfExists))
        .then([fileName](StorageFolder^ folder) { return folder->CreateFileAsync(fileName, CreationCollisionOption::ReplaceExisting); })
        .then([data](StorageFile^ file)
    {
        return create_task(FileIO::WriteBytesAsync(file, data)).then([file]() { return Windows::System::Launcher::LaunchFileAsync(file); });
    }).then([this](task<bool> t)
    {
        try { if (!t.get()) Log("no app to open the file"); }
        catch (Exception^ ex) { Log("open file: " + FromP(ex->Message)); }
    });
}

std::wstring TgService::SaveToDownloads(const std::string& bytes, const std::wstring& name)
{
    std::wstring dir = L"C:\\Data\\Users\\Public\\Downloads";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Telegram";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring clean;
    for (size_t i = 0; i < name.size(); ++i)
        clean += wcschr(L"\\/:*?\"<>|", name[i]) ? L'_' : name[i];
    if (clean.empty()) clean = L"file";
    std::wstring base = clean, ext;
    size_t dot = clean.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) { base = clean.substr(0, dot); ext = clean.substr(dot); }
    std::wstring path = dir + L"\\" + clean;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    for (int n = 1; GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && n < 1000; ++n)
        path = dir + L"\\" + base + L" (" + std::to_wstring(n) + L")" + ext;
    CREATEFILE2_EXTENDED_PARAMETERS p;
    ZeroMemory(&p, sizeof(p));
    p.dwSize = sizeof(p);
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_NEW, &p);
    if (h == INVALID_HANDLE_VALUE) { Log("save to Downloads failed " + std::to_string(static_cast<long long>(GetLastError()))); return std::wstring(); }
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != bytes.size()) { DeleteFileW(path.c_str()); return std::wstring(); }
    return path;
}
