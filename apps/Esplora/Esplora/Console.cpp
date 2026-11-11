#include "pch.h"
#include "Console.h"
#include "Core/Cmd.h"
#include "Core/Fs.h"
#include <sstream>
#include <windows.graphics.imaging.h>
#include <windows.security.cryptography.h>
#include <windows.storage.streams.h>
#include <windows.ui.xaml.media.imaging.h>

using namespace Platform;
using namespace Windows::Foundation;
using namespace concurrency;

namespace Esplora
{
    static const wchar_t* kBase = L"http://192.168.1.148:8765/";
    static bool g_on = false;
    static bool g_started = false;
    static bool g_busy = false;
    static std::wstring g_lastId;
    static Windows::UI::Xaml::DispatcherTimer^ g_timer;

    // the id of the last command run is kept in LocalSettings: reopening the app does not run it again
    static void RememberId(const std::wstring& id)
    {
        g_lastId = id;
        try { Windows::Storage::ApplicationData::Current->LocalSettings->Values->Insert(L"lastCmdId", ref new String(id.c_str())); }
        catch (...) {}
    }

    static void LoadLastId()
    {
        try
        {
            auto v = Windows::Storage::ApplicationData::Current->LocalSettings->Values->Lookup(L"lastCmdId");
            if (v != nullptr) g_lastId = safe_cast<String^>(v)->Data();
        }
        catch (...) {}
    }

    void PostLog(const std::wstring& name, const std::wstring& text)
    {
        try
        {
            auto client = ref new Windows::Web::Http::HttpClient();
            auto content = ref new Windows::Web::Http::HttpStringContent(ref new String(text.c_str()));
            auto uri = ref new Uri(ref new String((std::wstring(kBase) + name).c_str()));
            // the client must live until the upload ends, or big bodies are cut short
            create_task(client->PostAsync(uri, content)).then([client](task<Windows::Web::Http::HttpResponseMessage^> t) { try { t.get(); } catch (...) {} });
        }
        catch (...) {}
    }

    // "shot": the page as PNG, base64 in share/inbox/esplora-shot.txt (popups are not captured). UI thread only.
    static void TakeShot()
    {
        using namespace Windows::Graphics::Imaging;
        using namespace Windows::Storage::Streams;
        auto bitmap = ref new Windows::UI::Xaml::Media::Imaging::RenderTargetBitmap();
        create_task(bitmap->RenderAsync(Windows::UI::Xaml::Window::Current->Content)).then([bitmap]()
        {
            return bitmap->GetPixelsAsync();
        }).then([bitmap](IBuffer^ pixels)
        {
            auto stream = ref new InMemoryRandomAccessStream();
            return create_task(BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId, stream)).then([bitmap, pixels, stream](BitmapEncoder^ encoder)
            {
                auto bytes = ref new Array<unsigned char>(pixels->Length);
                DataReader::FromBuffer(pixels)->ReadBytes(bytes);
                encoder->SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied,
                    bitmap->PixelWidth, bitmap->PixelHeight, 96, 96, bytes);
                return create_task(encoder->FlushAsync());
            }).then([stream]()
            {
                stream->Seek(0);
                auto buffer = ref new Buffer(static_cast<unsigned int>(stream->Size));
                return create_task(stream->ReadAsync(buffer, buffer->Capacity, InputStreamOptions::None));
            });
        }).then([](task<IBuffer^> t)
        {
            try
            {
                PostLog(L"esplora-shot", Windows::Security::Cryptography::CryptographicBuffer::EncodeToBase64String(t.get())->Data());
            }
            catch (Exception^ ex) { PostLog(L"esplora-shot", std::wstring(L"ERRORE ") + ex->Message->Data()); }
        });
    }

    // "get <path>": the file to the PC, "<path>\n<base64>" in share/inbox/esplora-file.txt (read only).
    static void SendFile(const std::wstring& path)
    {
        create_task([path]()
        {
            std::vector<unsigned char> bytes;
            std::wstring error = Fs::ReadFile(path, bytes, 40 * 1024 * 1024);
            if (!error.empty()) { PostLog(L"esplora-file", path + L"\nERRORE " + error); return; }
            auto buffer = Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(
                ArrayReference<unsigned char>(bytes.data(), static_cast<unsigned int>(bytes.size())));
            PostLog(L"esplora-file", path + L"\n" + Windows::Security::Cryptography::CryptographicBuffer::EncodeToBase64String(buffer)->Data());
        });
    }

    // "open <uri>": opens the link like any app would (Launcher), to see which app the phone picks. UI thread only.
    static void OpenUri(const std::wstring& text)
    {
        try
        {
            auto uri = ref new Uri(ref new String(text.c_str()));
            create_task(Windows::System::Launcher::LaunchUriAsync(uri)).then([text](task<bool> t)
            {
                try { PostLog(L"esplora-out", L">>> open " + text + (t.get() ? L"\nOK aperto\n" : L"\nLaunchUriAsync = false\n")); }
                catch (Exception^ ex) { PostLog(L"esplora-out", L">>> open " + text + L"\nERRORE " + ex->Message->Data() + L"\n"); }
            });
        }
        catch (Exception^ ex) { PostLog(L"esplora-out", L">>> open " + text + L"\nERRORE " + ex->Message->Data() + L"\n"); }
    }

    // "put <name> <path>": downloads share/<name> from the PC and writes it to <path> (overwrites).
    static void PutFile(const std::wstring& args)
    {
        size_t sp = args.find(L' ');
        if (sp == std::wstring::npos) { PostLog(L"esplora-out", L">>> put " + args + L"\nuso: put <nome> <percorso>\n"); return; }
        std::wstring name = args.substr(0, sp), path = args.substr(sp + 1);
        try
        {
            auto client = ref new Windows::Web::Http::HttpClient();
            std::wstringstream u; u << kBase << name << L"?t=" << GetTickCount64();
            create_task(client->GetBufferAsync(ref new Uri(ref new String(u.str().c_str())))).then([client, args, path](task<Windows::Storage::Streams::IBuffer^> t)
            {
                try
                {
                    auto buffer = t.get();
                    Array<unsigned char>^ arr;
                    Windows::Security::Cryptography::CryptographicBuffer::CopyToByteArray(buffer, &arr);
                    std::vector<unsigned char> bytes(arr->Data, arr->Data + arr->Length);
                    std::wstring r = Fs::WriteFile(path, bytes);
                    std::wstringstream o; o << L">>> put " << args << L"\n" << (r.empty() ? L"OK " : L"ERRORE " + r + L" ") << bytes.size() << L" byte\n";
                    PostLog(L"esplora-out", o.str());
                }
                catch (Exception^ ex) { PostLog(L"esplora-out", L">>> put " + args + L"\nERRORE " + ex->Message->Data() + L"\n"); }
            });
        }
        catch (Exception^ ex) { PostLog(L"esplora-out", L">>> put " + args + L"\nERRORE " + ex->Message->Data() + L"\n"); }
    }

    static void Poll()
    {
        if (g_busy) return;
        g_busy = true;
        try
        {
            auto client = ref new Windows::Web::Http::HttpClient();
            std::wstringstream u; u << kBase << L"esplora-cmd.txt?t=" << GetTickCount64();
            create_task(client->GetStringAsync(ref new Uri(ref new String(u.str().c_str())))).then([](task<String^> t)
            {
                try
                {
                    std::wstring body = t.get()->Data();
                    std::wstringstream in(body);
                    std::wstring line, id;
                    std::vector<std::wstring> cmds;
                    bool first = true;
                    while (std::getline(in, line))
                    {
                        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
                        if (first) { first = false; if (!line.empty() && line[0] == L'#') { id = line; continue; } }
                        if (!line.empty()) cmds.push_back(line);
                    }
                    if (id.empty() || id == g_lastId || cmds.empty()) { g_busy = false; return; }
                    RememberId(id);
                    for (size_t i = 0; i < cmds.size(); i++)
                        if (cmds[i] == L"shot") { TakeShot(); cmds.erase(cmds.begin() + i); break; }
                    for (size_t i = 0; i < cmds.size(); )
                        if (cmds[i].compare(0, 4, L"put ") == 0) { PutFile(cmds[i].substr(4)); cmds.erase(cmds.begin() + i); }
                        else if (cmds[i].compare(0, 5, L"open ") == 0) { OpenUri(cmds[i].substr(5)); cmds.erase(cmds.begin() + i); }
                        else if (cmds[i].compare(0, 4, L"get ") == 0) { SendFile(cmds[i].substr(4)); cmds.erase(cmds.begin() + i); }
                        else ++i;
                    if (cmds.empty()) { g_busy = false; return; }
                    create_task([cmds, id]()
                    {
                        std::wstring all = L"##### " + id + L"\n";
                        for (size_t i = 0; i < cmds.size(); i++)
                        {
                            all += L">>> " + cmds[i] + L"\n";
                            try { all += Cmd::Run(cmds[i]); } catch (...) { all += L"eccezione\n"; }
                            all += L"\n";
                        }
                        PostLog(L"esplora-out", all);
                        g_busy = false;
                    });
                }
                catch (...) { g_busy = false; }
            });
        }
        catch (...) { g_busy = false; }
    }

    // USB channel, for when the phone cannot reach the PC: the PC drops usb\cmd.txt into LocalFolder with ISETool
    // (same format as esplora-cmd.txt), the answers are appended to usb\out.txt, "put <name> <path>" reads
    // usb\<name> and "get <path>" writes usb\get\<file name>.
    static std::wstring g_lastUsbId;

    static std::wstring UsbDir()
    {
        return std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\usb";
    }

    static void UsbAppend(const std::wstring& text)
    {
        std::wstring path = UsbDir() + L"\\out.txt";
        HANDLE file = CreateFile2(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &bytes[0], n, nullptr, nullptr);
        DWORD written = 0;
        ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
    }

    static void PollUsb()
    {
        std::wstring dir = UsbDir();
        std::vector<unsigned char> raw;
        if (!Fs::Exists(dir + L"\\cmd.txt")) return;
        if (!Fs::ReadFile(dir + L"\\cmd.txt", raw, 1024 * 1024).empty()) return;
        Fs::Delete(dir + L"\\cmd.txt", false);
        int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()), nullptr, 0);
        std::wstring body(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()), &body[0], n);

        std::wstringstream in(body);
        std::wstring line, id;
        std::vector<std::wstring> cmds;
        bool first = true;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
            if (first) { first = false; if (!line.empty() && line[0] == L'#') { id = line; continue; } }
            if (!line.empty()) cmds.push_back(line);
        }
        if (id.empty() || id == g_lastUsbId || cmds.empty()) return;
        g_lastUsbId = id;

        std::vector<std::wstring> rest;
        for (size_t i = 0; i < cmds.size(); i++)
            if (cmds[i].compare(0, 5, L"open ") == 0) OpenUri(cmds[i].substr(5));   // UI thread; answer only to the inbox
            else rest.push_back(cmds[i]);
        create_task([rest, id, dir]()
        {
            std::wstring all = L"##### " + id + L"\n";
            for (size_t i = 0; i < rest.size(); i++)
            {
                const std::wstring& c = rest[i];
                all += L">>> " + c + L"\n";
                try
                {
                    if (c.compare(0, 4, L"put ") == 0)
                    {
                        std::wstring args = c.substr(4);
                        size_t sp = args.find(L' ');
                        if (sp == std::wstring::npos) all += L"uso: put <nome in usb> <percorso>\n";
                        else
                        {
                            std::vector<unsigned char> bytes;
                            std::wstring r = Fs::ReadFile(dir + L"\\" + args.substr(0, sp), bytes, 64 * 1024 * 1024);
                            if (r.empty()) r = Fs::WriteFile(args.substr(sp + 1), bytes);
                            all += (r.empty() ? L"OK " + std::to_wstring(bytes.size()) + L" byte" : L"ERRORE " + r) + L"\n";
                        }
                    }
                    else if (c.compare(0, 4, L"get ") == 0)
                    {
                        std::wstring path = c.substr(4);
                        size_t slash = path.find_last_of(L"\\/");
                        std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
                        Fs::CreateDirectory(dir + L"\\get");
                        std::vector<unsigned char> bytes;
                        std::wstring r = Fs::ReadFile(path, bytes, 64 * 1024 * 1024);
                        if (r.empty()) r = Fs::WriteFile(dir + L"\\get\\" + name, bytes);
                        all += (r.empty() ? L"OK usb\\get\\" + name + L" " + std::to_wstring(bytes.size()) + L" byte" : L"ERRORE " + r) + L"\n";
                    }
                    else all += Cmd::Run(c);
                }
                catch (...) { all += L"eccezione\n"; }
                all += L"\n";
            }
            UsbAppend(all);
        });
    }

    void SetConsoleEnabled(bool on)
    {
        g_on = on;
        if (on && !g_started)
        {
            g_started = true;
            LoadLastId();
            Fs::CreateDirectory(UsbDir());
            g_timer = ref new Windows::UI::Xaml::DispatcherTimer();
            TimeSpan ts; ts.Duration = 30000000LL; // 3 s
            g_timer->Interval = ts;
            g_timer->Tick += ref new EventHandler<Object^>([](Object^, Object^) { if (g_on) { Poll(); PollUsb(); } });
            g_timer->Start();
        }
    }
    bool IsConsoleEnabled() { return g_on; }

    void SkipPendingCommand(std::function<void(std::wstring)> done)
    {
        try
        {
            auto client = ref new Windows::Web::Http::HttpClient();
            std::wstringstream u; u << kBase << L"esplora-cmd.txt?t=" << GetTickCount64();
            create_task(client->GetStringAsync(ref new Uri(ref new String(u.str().c_str())))).then([client, done](task<String^> t)
            {
                std::wstring body;
                try { body = t.get()->Data(); }
                catch (Exception^) { done(L"PC non raggiungibile"); return; }
                std::wstringstream in(body);
                std::wstring id, line, cmds;
                std::getline(in, id);
                while (!id.empty() && (id.back() == L'\r' || id.back() == L'\n')) id.pop_back();
                while (std::getline(in, line)) if (!line.empty()) cmds += (cmds.empty() ? L"" : L"; ") + line;
                if (id.empty() || id[0] != L'#') { done(L"nessun comando in attesa"); return; }
                bool already = id == g_lastId;
                RememberId(id);
                done(already ? L"nessun comando in attesa (l'ultimo era gi\u00e0 eseguito)" : L"ignorato: " + cmds);
            }, task_continuation_context::use_current());
        }
        catch (...) { done(L"errore"); }
    }
}
