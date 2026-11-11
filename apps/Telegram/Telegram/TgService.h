#pragma once

// TgService: the one Telegram engine + client of the app, living on the UI thread. Owns the sockets
// (one StreamSocket per data center), the 500 ms tick, state files in LocalFolder, the media cache and
// in-app toasts. Pages register as listeners to get chat/message events.

#include "client.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Telegram
{
    ref class Conn;

    class TgService : public tg::IEngineHost, public tg::IClientListener
    {
    public:
        static TgService& Get();

        void Init(Windows::UI::Core::CoreDispatcher^ dispatcher);
        bool Ready() const { return engine_ != nullptr; }
        tg::Client& Client() { return *client_; }
        tg::Engine& Engine() { return *engine_; }
        void Suspend();
        void Resume();

        void AddListener(tg::IClientListener* l);
        void RemoveListener(tg::IClientListener* l);

        // Chat currently on screen: no toast for it, and it is marked read on arrival.
        std::string openChat;

        // File from the media cache (LocalFolder\cache) or downloaded and then cached.
        void GetMedia(const std::string& key, const std::function<void(const std::string& bytes, bool ok)>& done,
                      const std::function<void(int64_t, int64_t)>& progress = std::function<void(int64_t, int64_t)>());
        // Writes bytes to LocalFolder\Downloads\name and opens it with the default app (video, documents...).
        void OpenWithSystem(const std::string& bytes, const std::wstring& name);

        // Saves to C:\Data\Users\Public\Downloads\Telegram (the phone is rooted: apps can write there).
        // Returns the full path, or "" on error. Unique name if the file exists.
        std::wstring SaveToDownloads(const std::string& bytes, const std::wstring& name);
        // File picked in FilePage, sent by ChatPage when it comes back on screen.
        std::wstring pendingAttach;
        // Files selected together in FilePage; ChatPage previews the batch before sending it.
        std::vector<std::wstring> pendingAttachments;
        // Messages chosen with "Inoltra" in ChatPage, sent by NewChatPage to the chat picked there.
        std::string pendingForwardFrom;
        std::vector<int32_t> pendingForwardIds;

        void SaveSoon();
        std::string LogText() const;
        void SendLog();                 // POST the log to the PC inbox (tools/inbox), for diagnostics
        void Log(const std::string& line);

        // IEngineHost
        virtual void netConnect(int conn, const std::string& host, int port);
        virtual void netSend(int conn, const std::vector<uint8_t>& data);
        virtual void netClose(int conn);
        virtual void randomBytes(uint8_t* out, std::size_t n);
        virtual int64_t unixTimeMs();
        virtual void onUpdates(const tg::TlValue& updates);
        virtual void onSessionReset();
        virtual void onStateChanged();
        virtual void onAuthLost();
        virtual void onOnline(bool online);
        virtual void log(const std::string& line) { Log(line); }

        // IClientListener (fan-out to pages + toasts)
        virtual void onAuthState(int state);
        virtual void onChatsChanged();
        virtual void onNewMessage(const tg::MsgItem& m);
        virtual void onMessageEdited(const tg::MsgItem& m);
        virtual void onMessagesDeleted(const std::string& chatKey, const std::vector<int32_t>& ids);
        virtual void onReadOutbox(const std::string& chatKey, int32_t maxId);
        virtual void onError(const std::string& what);

        bool online;

    private:
        TgService();
        void Tick();
        void SaveNow();
        void Toast(const std::string& title, const std::string& text, const std::string& chatKey);
        void ReadLoop(int dc, Conn^ c);
        void Flush(int dc, Conn^ c);

        Windows::UI::Core::CoreDispatcher^ dispatcher_;
        Windows::UI::Xaml::DispatcherTimer^ timer_;
        tg::Engine* engine_;
        tg::Client* client_;
        std::map<int, Conn^> conns_;
        std::vector<tg::IClientListener*> listeners_;
        std::vector<std::string> log_;
        bool saveDue_;
        int saveTicks_;
        bool chatsDirty_;
        int logTicks_;
        size_t logSent_;
        int fgTicks_;
        static const int ForegroundTicks = 60;      // 30 s of 500 ms ticks between tg.fg refreshes
    };

    // UTF-8 <-> UTF-16 helpers for the pages
    std::wstring Utf16(const std::string& s);
    std::string Utf8(const std::wstring& w);
    Platform::String^ PStr(const std::string& s);
    std::string FromP(Platform::String^ s);

    // decodes JPEG/PNG bytes into a bitmap on the UI thread; done(nullptr) if WP8.1 cannot decode them (e.g. WebP)
    void DecodeImage(const std::string& bytes, std::function<void(Windows::UI::Xaml::Media::Imaging::BitmapImage^)> done);
}
