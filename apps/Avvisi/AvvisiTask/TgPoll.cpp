#define NOMINMAX
#include "TgPoll.h"
#include "Shared.h"
#include "ApiKeys.h"
#include "client.h"

#include <windows.h>
#include <ppltasks.h>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace AvvisiTask;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace AvvisiTask
{
    ref class TgConn sealed
    {
    internal:
        StreamSocket^ socket;
        DataWriter^ writer;
        DataReader^ reader;
        std::vector<uint8_t> pending;
        bool storing;
        bool closed;
        TgConn() : storing(false), closed(false) {}
    };
}

namespace
{
    const int PollSeconds = 30;
    const unsigned int FullCheckEvery = 15;     // dialogs at least every 15 polls (30 min) even if getState is unchanged
    const long long ForegroundMs = 75000;       // the Telegram app writes tg.fg every 30 s while it runs

    std::wstring Utf16(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
        return w;
    }

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::wstring LowerAscii(std::wstring s)
    {
        for (size_t i = 0; i < s.size(); ++i) if (s[i] >= L'A' && s[i] <= L'Z') s[i] = s[i] - L'A' + L'a';
        return s;
    }

    // Replaces `from` with `to` in `path`, ignoring ASCII case (phone paths come in mixed case).
    bool ReplaceNoCase(std::wstring& path, const std::wstring& from, const std::wstring& to)
    {
        size_t at = LowerAscii(path).find(LowerAscii(from));
        if (at == std::wstring::npos) return false;
        path = path.substr(0, at) + to + path.substr(at + from.size());
        return true;
    }

    DWORD ReadAll(const std::wstring& path, std::string& out)
    {
        out.clear();
        HANDLE f = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
        if (f == INVALID_HANDLE_VALUE) return GetLastError();
        FILE_STANDARD_INFO info = {};
        GetFileInformationByHandleEx(f, FileStandardInfo, &info, sizeof(info));
        out.assign(static_cast<size_t>(info.EndOfFile.QuadPart), '\0');
        DWORD read = 0;
        BOOL ok = out.empty() ? TRUE : ::ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
        DWORD error = ok ? 0 : GetLastError();
        CloseHandle(f);
        out.resize(read);
        return error;
    }

    void WriteAll(const std::wstring& path, const std::string& data)
    {
        std::wstring tmp = path + L".tmp";
        HANDLE f = CreateFile2(tmp.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        if (!data.empty()) ::WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(f);
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    std::wstring OwnPath(const wchar_t* name)
    {
        return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + name;
    }

    int GetInt(const wchar_t* key, int fallback)
    {
        try
        {
            IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
            String^ k = ref new String(key);
            return values->HasKey(k) ? safe_cast<int>(values->Lookup(k)) : fallback;
        }
        catch (...) { return fallback; }
    }

    void SetInt(const wchar_t* key, int value)
    {
        try { ApplicationData::Current->LocalSettings->Values->Insert(ref new String(key), value); }
        catch (...) { }
    }

    // "key topId" lines: the newest message of each chat already looked at (notified or not)
    bool LoadSeen(std::map<std::string, int32_t>& seen)
    {
        std::string text;
        if (ReadAll(OwnPath(L"tg.seen"), text) != 0) return false;
        std::istringstream in(text);
        std::string key;
        long long id = 0;
        while (in >> key >> id) seen[key] = static_cast<int32_t>(id);
        return true;
    }

    void SaveSeen(const std::map<std::string, int32_t>& seen)
    {
        std::ostringstream out;
        for (std::map<std::string, int32_t>::const_iterator it = seen.begin(); it != seen.end(); ++it)
            out << it->first << " " << it->second << "\n";
        WriteAll(OwnPath(L"tg.seen"), out.str());
    }

    // Engine host for one poll at a time. The engine is used under `mu` only: from the polling thread (tick) and
    // from the socket continuations (thread pool).
    class Poller : public tg::IEngineHost
    {
    public:
        static Poller& Get()
        {
            static Poller* p = new Poller();
            return *p;
        }

        std::wstring Poll()
        {
            std::wstring folder = TelegramFolder();
            std::string state;
            DWORD error = ReadAll(folder + L"\\tg.state", state);
            if (error != 0 || state.empty())
                return L"telegram: sessione non trovata (errore " + std::to_wstring(error) + L")";
            if (state.find("authorized 1") == std::string::npos) return L"telegram: l'app non ha fatto l'accesso";

            std::string fg;
            bool foreground = ReadAll(folder + L"\\tg.fg", fg) == 0 && NowMs() - _atoi64(fg.c_str()) < ForegroundMs;

            {
                std::lock_guard<std::recursive_mutex> lock(mu_);
                if (engine_ == nullptr || state != blob_) Reset(state);
                ++gen_;
                done_ = false;
                status_.clear();
                foreground_ = foreground;
                lines_.clear();
                engine_->start();
                GetState(gen_);
            }

            if (wait_ == nullptr) wait_ = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            ULONGLONG until = GetTickCount64() + PollSeconds * 1000;
            std::lock_guard<std::recursive_mutex> lock(mu_);
            for (;;)
            {
                mu_.unlock();
                WaitForSingleObjectEx(wait_, 200, FALSE);
                mu_.lock();
                engine_->tick();
                if (done_) break;
                if (GetTickCount64() > until)
                {
                    status_ = L"telegram: nessuna risposta in " + std::to_wstring(PollSeconds) + L" s" + LastLines();
                    blob_.clear();          // next poll: fresh engine, without this poll's requests
                    break;
                }
            }
            engine_->stop();
            return status_;
        }

        // IEngineHost (called with mu_ held)
        virtual void netConnect(int dc, const std::string& host, int port)
        {
            netClose(dc);
            TgConn^ c = ref new TgConn();
            c->socket = ref new StreamSocket();
            c->socket->Control->NoDelay = true;
            conns_[dc] = c;
            concurrency::create_task(c->socket->ConnectAsync(ref new HostName(ref new String(Utf16(host).c_str())),
                ref new String(std::to_wstring(port).c_str()))).then([this, dc, c](concurrency::task<void> t)
            {
                std::lock_guard<std::recursive_mutex> lock(mu_);
                try { t.get(); }
                catch (Exception^ ex) { if (!c->closed) Drop(dc, c, "connect " + std::to_string(ex->HResult)); return; }
                catch (...) { if (!c->closed) Drop(dc, c, "connect"); return; }
                if (c->closed) return;
                c->writer = ref new DataWriter(c->socket->OutputStream);
                c->reader = ref new DataReader(c->socket->InputStream);
                c->reader->InputStreamOptions = InputStreamOptions::Partial;
                engine_->onConnected(dc);
                Flush(dc, c);
                ReadLoop(dc, c);
            });
        }

        virtual void netSend(int dc, const std::vector<uint8_t>& data)
        {
            std::map<int, TgConn^>::iterator it = conns_.find(dc);
            if (it == conns_.end() || data.empty()) return;
            TgConn^ c = it->second;
            c->pending.insert(c->pending.end(), data.begin(), data.end());
            if (c->writer != nullptr) Flush(dc, c);
        }

        virtual void netClose(int dc)
        {
            std::map<int, TgConn^>::iterator it = conns_.find(dc);
            if (it == conns_.end()) return;
            TgConn^ c = it->second;
            conns_.erase(it);
            c->closed = true;
            try { delete c->socket; } catch (...) { }
        }

        virtual void randomBytes(uint8_t* out, std::size_t n)
        {
            IBuffer^ b = CryptographicBuffer::GenerateRandom(static_cast<unsigned int>(n));
            Array<unsigned char>^ a;
            CryptographicBuffer::CopyToByteArray(b, &a);
            memcpy(out, a->Data, n);
        }

        virtual int64_t unixTimeMs() { return NowMs(); }
        virtual void onAuthLost() { Finish(L"telegram: sessione non pi\u00f9 valida (rifare l'accesso nell'app)"); }
        virtual void log(const std::string& line)
        {
            lines_.push_back(line);
            if (lines_.size() > 6) lines_.erase(lines_.begin());
        }

    private:
        Poller() : engine_(nullptr), client_(nullptr), gen_(0), done_(false), foreground_(false), wait_(nullptr) {}

        void Reset(const std::string& state)
        {
            for (std::map<int, TgConn^>::iterator it = conns_.begin(); it != conns_.end(); ++it)
            {
                it->second->closed = true;
                try { delete it->second->socket; } catch (...) { }
            }
            conns_.clear();
            delete client_;
            delete engine_;
            tg::EngineConfig cfg;
            cfg.apiId = TG_API_ID;
            cfg.deviceModel = "Lumia 1020";
            cfg.systemVersion = "Windows Phone 8.1";
            cfg.appVersion = "3.0";
            cfg.langCode = "it";
            engine_ = new tg::Engine(this, cfg);
            client_ = new tg::Client(engine_, TG_API_ID, TG_API_HASH);
            engine_->load(state);
            blob_ = state;
        }

        void Drop(int dc, TgConn^ c, const std::string& why)
        {
            c->closed = true;
            std::map<int, TgConn^>::iterator it = conns_.find(dc);
            if (it != conns_.end() && it->second == c) conns_.erase(it);
            if (!why.empty()) log("net " + why);
            engine_->onClosed(dc);
        }

        void ReadLoop(int dc, TgConn^ c)
        {
            concurrency::create_task(c->reader->LoadAsync(65536)).then([this, dc, c](concurrency::task<unsigned int> t)
            {
                std::lock_guard<std::recursive_mutex> lock(mu_);
                unsigned int n = 0;
                try { n = t.get(); }
                catch (...) { n = 0; }
                if (c->closed) return;
                if (n == 0) { Drop(dc, c, std::string()); return; }
                std::vector<uint8_t> buf(n);
                c->reader->ReadBytes(ArrayReference<unsigned char>(&buf[0], n));
                engine_->onData(dc, &buf[0], n);
                if (!c->closed) ReadLoop(dc, c);
            });
        }

        void Flush(int dc, TgConn^ c)
        {
            if (c->closed || c->storing || c->pending.empty() || c->writer == nullptr) return;
            c->storing = true;
            std::vector<uint8_t> out;
            out.swap(c->pending);
            c->writer->WriteBytes(ArrayReference<unsigned char>(&out[0], static_cast<unsigned int>(out.size())));
            concurrency::create_task(c->writer->StoreAsync()).then([this, dc, c](concurrency::task<unsigned int> t)
            {
                std::lock_guard<std::recursive_mutex> lock(mu_);
                c->storing = false;
                try { t.get(); }
                catch (...) { if (!c->closed) Drop(dc, c, "write"); return; }
                Flush(dc, c);
            });
        }

        void Finish(const std::wstring& status)
        {
            if (done_) return;
            status_ = status;
            done_ = true;
        }

        std::wstring LastLines() const
        {
            std::wstring out;
            for (size_t i = 0; i < lines_.size(); ++i) out += L" | " + Utf16(lines_[i]);
            return out;
        }

        void GetState(int gen)
        {
            engine_->invoke(tg::tlObj("updates.getState"), [this, gen](const tg::TlValue& s, const tg::RpcErr& e)
            {
                if (gen != gen_) return;
                if (!e.ok()) { Finish(L"telegram: errore " + Utf16(e.message) + LastLines()); return; }
                int pts = s["pts"].asInt(), unread = s["unread_count"].asInt();
                int oldPts = GetInt(L"tgPts", -1), oldUnread = GetInt(L"tgUnread", -1);
                int polls = GetInt(L"tgPolls", 0) + 1;
                SetInt(L"tgPts", pts);
                SetInt(L"tgUnread", unread);
                SetInt(L"tgPolls", polls);
                std::map<std::string, int32_t> seen;
                bool changed = pts != oldPts || unread != oldUnread;
                if (!changed && polls % FullCheckEvery != 0 && LoadSeen(seen))
                {
                    Finish(L"telegram: niente di nuovo, " + std::to_wstring(unread) + L" non letti");
                    return;
                }
                client_->loadChats([this, gen, unread](const tg::RpcErr& e2)
                {
                    if (gen != gen_) return;
                    if (!e2.ok()) { Finish(L"telegram: errore chat " + Utf16(e2.message)); return; }
                    Finish(Notify(unread));
                }, 20);
            });
        }

        std::wstring Notify(int unread)
        {
            std::map<std::string, int32_t> seen;
            bool first = !LoadSeen(seen);
            const std::vector<tg::ChatItem>& chats = client_->chats();
            std::vector<const tg::ChatItem*> fresh;
            for (size_t i = 0; i < chats.size(); ++i)
            {
                const tg::ChatItem& c = chats[i];
                std::map<std::string, int32_t>::iterator it = seen.find(c.key);
                int32_t last = it == seen.end() ? 0 : it->second;
                if (c.topId <= last) continue;
                if (!first && !c.lastOut && c.unread > 0 && !c.muted) fresh.push_back(&c);
                seen[c.key] = c.topId;
            }
            SaveSeen(seen);
            std::wstring tail = L", " + std::to_wstring(unread) + L" non letti";
            if (first) return L"telegram: pronto, " + std::to_wstring(chats.size()) + L" chat" + tail;
            if (fresh.empty()) return L"telegram: nessun messaggio nuovo" + tail;
            if (foreground_) return L"telegram: " + std::to_wstring(fresh.size()) + L" chat nuove, app aperta: niente avviso";

            std::wstring names;
            for (size_t i = 0; i < fresh.size() && i < 3; ++i)
            {
                const tg::ChatItem& c = *fresh[i];
                std::wstring text = Utf16(c.lastText);
                if (c.unread > 1) text = L"(" + std::to_wstring(c.unread) + L") " + text;
                Toast(Utf16(c.title), text, false, L"chat=" + Utf16(c.key));
                names += (names.empty() ? L"" : L", ") + Utf16(c.title);
            }
            if (fresh.size() > 3)
                Toast(L"Telegram", L"messaggi nuovi in altre " + std::to_wstring(fresh.size() - 3) + L" chat", true, L"tg:");
            return L"telegram: avviso per " + names + tail;
        }

        std::recursive_mutex mu_;
        tg::Engine* engine_;
        tg::Client* client_;
        std::string blob_;
        std::map<int, TgConn^> conns_;
        std::vector<std::string> lines_;
        int gen_;
        bool done_;
        bool foreground_;
        std::wstring status_;
        HANDLE wait_;
    };
}

std::wstring AvvisiTask::TelegramFolder()
{
    std::wstring path = ApplicationData::Current->LocalFolder->Path->Data();
    std::wstring family = Package::Current->Id->FamilyName->Data();      // DumbPhone.Avvisi_<publisher hash>
    size_t underscore = family.find(L'_');
    if (underscore != std::wstring::npos &&
        ReplaceNoCase(path, family, L"DumbPhone.TelegramWeb" + family.substr(underscore)))
        return path;
    // product id folders (C:\Data\Users\DefApps\AppData\{id}\...)
    ReplaceNoCase(path, L"23935f1f-29b5-49e5-958e-016746eb689c", L"c5e1671e-09b6-4b94-98db-0bc5d1a47637");
    return path;
}

std::wstring AvvisiTask::PollTelegram()
{
    try { return Poller::Get().Poll(); }
    catch (Exception^ ex) { return L"telegram: errore " + Hex(ex->HResult); }
    catch (const std::exception& ex) { return L"telegram: errore " + Utf16(ex.what()); }
    catch (...) { return L"telegram: errore"; }
}
