#define NOMINMAX
#include "MailPoll.h"
#include "Shared.h"

#include <windows.h>
#include <ppltasks.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace PostaTask;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Credentials;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System::Threading;

namespace
{
    const int TimeoutSeconds = 20;
    const size_t MaxToasts = 3;

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    // Bytes in a given charset to UTF-16; UTF-8 when the charset is unknown, Latin-1 when the bytes are not UTF-8.
    std::wstring Decode(const std::string& bytes, std::string charset)
    {
        if (bytes.empty()) return std::wstring();
        std::transform(charset.begin(), charset.end(), charset.begin(), ::tolower);
        UINT cp = CP_UTF8;
        if (charset == "iso-8859-1" || charset == "latin1" || charset == "us-ascii") cp = 0;
        else if (charset == "windows-1252" || charset == "cp1252") cp = 1252;
        else if (charset == "iso-8859-15") cp = 28605;
        if (cp != 0)
        {
            int n = MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (n > 0)
            {
                std::wstring w(n, L'\0');
                MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes.data(), static_cast<int>(bytes.size()), &w[0], n);
                return w;
            }
        }
        std::wstring w;
        for (size_t i = 0; i < bytes.size(); ++i) w += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]));
        return w;
    }

    int HexDigit(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string Base64(const std::string& in)
    {
        std::string out;
        int bits = 0, value = 0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            char c = in[i];
            int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
                    c == '+' ? 62 : c == '/' ? 63 : -1;
            if (d < 0) continue;
            value = (value << 6) | d;
            bits += 6;
            if (bits >= 8) { bits -= 8; out += static_cast<char>((value >> bits) & 0xFF); }
        }
        return out;
    }

    // RFC 2047 encoded words (=?charset?B|Q?text?=); the whitespace between two encoded words is dropped.
    std::wstring DecodeHeader(const std::string& raw)
    {
        std::wstring out;
        size_t at = 0;
        bool lastEncoded = false;
        while (at < raw.size())
        {
            size_t start = raw.find("=?", at);
            if (start == std::string::npos) { out += Decode(raw.substr(at), "utf-8"); break; }
            size_t q1 = raw.find('?', start + 2);
            size_t q2 = q1 == std::string::npos ? q1 : raw.find('?', q1 + 1);
            size_t end = q2 == std::string::npos ? q2 : raw.find("?=", q2 + 1);
            if (end == std::string::npos) { out += Decode(raw.substr(at), "utf-8"); break; }
            std::string between = raw.substr(at, start - at);
            if (!(lastEncoded && between.find_first_not_of(" \t") == std::string::npos)) out += Decode(between, "utf-8");
            std::string charset = raw.substr(start + 2, q1 - start - 2);
            size_t star = charset.find('*');                       // RFC 2231 language suffix
            if (star != std::string::npos) charset = charset.substr(0, star);
            char enc = static_cast<char>(toupper(raw[q1 + 1]));
            std::string text = raw.substr(q2 + 1, end - q2 - 1), bytes;
            if (enc == 'B') bytes = Base64(text);
            else
            {
                for (size_t i = 0; i < text.size(); ++i)
                {
                    if (text[i] == '_') bytes += ' ';
                    else if (text[i] == '=' && i + 2 < text.size() && HexDigit(text[i + 1]) >= 0 && HexDigit(text[i + 2]) >= 0)
                    {
                        bytes += static_cast<char>(HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2]));
                        i += 2;
                    }
                    else bytes += text[i];
                }
            }
            out += Decode(bytes, charset);
            at = end + 2;
            lastEncoded = true;
        }
        return out;
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n\""), b = s.find_last_not_of(L" \t\r\n\"");
        return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    }

    // "Name <addr>" -> Name; "<addr>" or "addr" -> addr
    std::wstring SenderName(const std::wstring& from)
    {
        size_t lt = from.find(L'<');
        if (lt == std::wstring::npos) return Trim(from);
        std::wstring name = Trim(from.substr(0, lt));
        if (!name.empty()) return name;
        size_t gt = from.find(L'>', lt);
        return Trim(from.substr(lt + 1, gt == std::wstring::npos ? std::wstring::npos : gt - lt - 1));
    }

    // From and Subject out of a header block (folded lines joined).
    void ParseHeaders(const std::string& block, std::wstring& from, std::wstring& subject)
    {
        std::vector<std::string> lines;
        size_t at = 0;
        while (at < block.size())
        {
            size_t end = block.find("\r\n", at);
            if (end == std::string::npos) end = block.size();
            std::string line = block.substr(at, end - at);
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
                lines.back() += " " + line.substr(line.find_first_not_of(" \t"));
            else if (!line.empty())
                lines.push_back(line);
            at = end + 2;
        }
        for (size_t i = 0; i < lines.size(); ++i)
        {
            size_t colon = lines[i].find(':');
            if (colon == std::string::npos) continue;
            std::string name = lines[i].substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            std::string value = lines[i].substr(colon + 1);
            if (name == "from") from = SenderName(DecodeHeader(value));
            else if (name == "subject") subject = Trim(DecodeHeader(value));
        }
    }

    unsigned long Field(const std::string& text, const char* name)
    {
        size_t at = text.find(name);
        return at == std::string::npos ? 0 : strtoul(text.c_str() + at + strlen(name), nullptr, 10);
    }

    std::string Quote(const std::string& s)
    {
        std::string out = "\"";
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '"' || s[i] == '\\') out += '\\';
            out += s[i];
        }
        return out + "\"";
    }

    unsigned int GetUInt(const wchar_t* key)
    {
        try
        {
            IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
            String^ k = ref new String(key);
            return values->HasKey(k) ? safe_cast<unsigned int>(values->Lookup(k)) : 0;
        }
        catch (...) { return 0; }
    }

    void SetUInt(const wchar_t* key, unsigned int value)
    {
        try { ApplicationData::Current->LocalSettings->Values->Insert(ref new String(key), value); }
        catch (...) { }
    }

    // Minimal blocking IMAP client: tagged commands, untagged answers with their literals inlined.
    class Imap
    {
    public:
        Imap() : tag_(0) {}
        ~Imap()
        {
            try { delete socket_; } catch (...) { }
        }

        std::wstring Connect(const wchar_t* host, const wchar_t* port)
        {
            socket_ = ref new StreamSocket();
            try
            {
                auto op = socket_->ConnectAsync(ref new HostName(ref new String(host)), ref new String(port), SocketProtectionLevel::Ssl);
                ThreadPoolTimer^ timeout = CancelAfter(op, TimeoutSeconds);
                concurrency::create_task(op).get();
                timeout->Cancel();
            }
            catch (const concurrency::task_canceled&) { return L"connessione scaduta"; }
            catch (Exception^ ex) { return L"connessione fallita " + Hex(ex->HResult); }
            reader_ = ref new DataReader(socket_->InputStream);
            reader_->InputStreamOptions = InputStreamOptions::Partial;
            writer_ = ref new DataWriter(socket_->OutputStream);
            return std::wstring();
        }

        // One response, with {n} literals and the line that follows them joined in.
        bool Response(std::string& out)
        {
            out.clear();
            for (;;)
            {
                std::string line;
                if (!Line(line)) return false;
                out += line;
                size_t open = line.rfind('{');
                if (line.empty() || line[line.size() - 1] != '}' || open == std::string::npos) return true;
                size_t n = strtoul(line.c_str() + open + 1, nullptr, 10);
                std::string literal;
                if (!Bytes(n, literal)) return false;
                out += "\r\n" + literal;
            }
        }

        // true for a tagged OK; status = the tagged line after the tag
        bool Command(const std::string& command, std::vector<std::string>& untagged, std::string& status)
        {
            untagged.clear();
            std::string tag = "a" + std::to_string(++tag_);
            if (!Send(tag + " " + command + "\r\n")) { status = "invio fallito"; return false; }
            for (;;)
            {
                std::string r;
                if (!Response(r)) { status = "connessione chiusa"; return false; }
                if (r.compare(0, tag.size() + 1, tag + " ") == 0)
                {
                    status = r.substr(tag.size() + 1);
                    return status.compare(0, 2, "OK") == 0;
                }
                untagged.push_back(r);
            }
        }

    private:
        bool Fill()
        {
            try
            {
                auto op = reader_->LoadAsync(16384);
                ThreadPoolTimer^ timeout = CancelAfter(op, TimeoutSeconds);
                unsigned int n = concurrency::create_task(op).get();
                timeout->Cancel();
                if (n == 0) return false;
                std::vector<unsigned char> b(n);
                reader_->ReadBytes(ArrayReference<unsigned char>(&b[0], n));
                buffer_.append(reinterpret_cast<char*>(&b[0]), n);
                return true;
            }
            catch (...) { return false; }
        }

        bool Line(std::string& line)
        {
            size_t end;
            while ((end = buffer_.find("\r\n")) == std::string::npos)
                if (buffer_.size() > (1u << 20) || !Fill()) return false;
            line = buffer_.substr(0, end);
            buffer_.erase(0, end + 2);
            return true;
        }

        bool Bytes(size_t n, std::string& out)
        {
            while (buffer_.size() < n)
                if (!Fill()) return false;
            out = buffer_.substr(0, n);
            buffer_.erase(0, n);
            return true;
        }

        bool Send(const std::string& text)
        {
            try
            {
                writer_->WriteBytes(ArrayReference<unsigned char>(reinterpret_cast<unsigned char*>(const_cast<char*>(text.data())),
                    static_cast<unsigned int>(text.size())));
                auto op = writer_->StoreAsync();
                ThreadPoolTimer^ timeout = CancelAfter(op, TimeoutSeconds);
                concurrency::create_task(op).get();
                timeout->Cancel();
                return true;
            }
            catch (...) { return false; }
        }

        StreamSocket^ socket_;
        DataReader^ reader_;
        DataWriter^ writer_;
        std::string buffer_;
        int tag_;
    };

    std::wstring Short(const std::string& status)
    {
        return Decode(status.substr(0, 80), "utf-8");
    }

    struct NewMail
    {
        unsigned long uid;
        std::wstring from, subject;
    };
}

namespace
{
    // A string or number field of the account JSON, "" when missing.
    std::wstring JsonText(JsonObject^ o, String^ name)
    {
        if (!o->HasKey(name)) return std::wstring();
        IJsonValue^ v = o->GetNamedValue(name);
        if (v->ValueType == JsonValueType::String) return Trim(v->GetString()->Data());
        if (v->ValueType == JsonValueType::Number) return std::to_wstring(static_cast<long long>(v->GetNumber()));
        return std::wstring();
    }
}

bool PostaTask::MailAccount(std::wstring& host, std::wstring& port, std::wstring& user, std::wstring& password)
{
    try
    {
        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
        String^ raw = values->HasKey("posta.account.v1") ? dynamic_cast<String^>(values->Lookup("posta.account.v1")) : nullptr;
        if (raw == nullptr || raw->IsEmpty()) return false;
        JsonObject^ account = JsonObject::Parse(raw);
        std::wstring email = JsonText(account, "Email");
        if (email.empty()) return false;
        host = JsonText(account, "ImapHost");
        port = JsonText(account, "ImapPort");
        user = JsonText(account, "Username");
        if (host.empty()) host = L"imap.gmail.com";
        if (port.empty() || port == L"0") port = L"993";
        if (user.empty()) user = email;
        PasswordVault^ vault = ref new PasswordVault();
        PasswordCredential^ credential = vault->Retrieve("Posta.Mail", ref new String(email.c_str()));
        credential->RetrievePassword();
        password = credential->Password->Data();
        return !password.empty();
    }
    catch (...) { return false; }
}

std::wstring PostaTask::PollMail()
{
    std::wstring host, port, user, password;
    if (!MailAccount(host, port, user, password)) return L"posta: account non configurato nell'app";

    Imap imap;
    std::wstring error = imap.Connect(host.c_str(), port.c_str());
    if (!error.empty()) return L"posta: " + error;
    std::string greeting, status;
    std::vector<std::string> lines;
    if (!imap.Response(greeting)) return L"posta: il server non risponde";
    if (!imap.Command("LOGIN " + Quote(Utf8(user)) + " " + Quote(Utf8(password)), lines, status))
        return L"posta: accesso rifiutato (" + Short(status) + L")";
    if (!imap.Command("STATUS INBOX (UIDNEXT UIDVALIDITY UNSEEN)", lines, status))
        return L"posta: STATUS fallito (" + Short(status) + L")";
    unsigned long next = 0, validity = 0, unseen = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].compare(0, 8, "* STATUS") != 0) continue;
        next = Field(lines[i], "UIDNEXT ");
        validity = Field(lines[i], "UIDVALIDITY ");
        unseen = Field(lines[i], "UNSEEN ");
    }
    unsigned int savedNext = GetUInt(L"mailNext"), savedValidity = GetUInt(L"mailValidity");
    std::wstring tail = std::to_wstring(unseen) + L" non lette";
    if (next == 0)
    {
        imap.Command("LOGOUT", lines, status);
        return L"posta: risposta STATUS senza UIDNEXT";
    }
    if (savedNext == 0 || savedValidity != validity)
    {
        SetUInt(L"mailNext", next);
        SetUInt(L"mailValidity", validity);
        imap.Command("LOGOUT", lines, status);
        return L"posta: pronta, " + tail;
    }
    if (next <= savedNext)
    {
        imap.Command("LOGOUT", lines, status);
        return L"posta: niente di nuovo, " + tail;
    }

    // new mail since the last check: the unread ones among the new UIDs
    std::vector<unsigned long> uids;
    if (imap.Command("EXAMINE INBOX", lines, status) &&
        imap.Command("UID SEARCH UID " + std::to_string(savedNext) + ":* UNSEEN", lines, status))
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].compare(0, 8, "* SEARCH") != 0) continue;
            const char* p = lines[i].c_str() + 8;
            char* end = nullptr;
            for (unsigned long uid = strtoul(p, &end, 10); end != p; p = end, uid = strtoul(p, &end, 10))
                if (uid >= savedNext) uids.push_back(uid);
        }
    }
    SetUInt(L"mailNext", next);
    if (uids.empty())
    {
        imap.Command("LOGOUT", lines, status);
        return L"posta: " + std::to_wstring(next - savedNext) + L" nuove, gi\u00e0 lette, " + tail;
    }
    if (uids.size() > 5) uids.erase(uids.begin(), uids.end() - 5);
    std::string set;
    for (size_t i = 0; i < uids.size(); ++i) set += (i ? "," : "") + std::to_string(uids[i]);
    std::vector<NewMail> mails;
    if (imap.Command("UID FETCH " + set + " (UID BODY.PEEK[HEADER.FIELDS (FROM SUBJECT)])", lines, status))
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            size_t body = lines[i].find("\r\n");
            if (lines[i].find("FETCH") == std::string::npos || body == std::string::npos) continue;
            NewMail m;
            m.uid = Field(lines[i], "UID ");
            ParseHeaders(lines[i].substr(body + 2), m.from, m.subject);
            mails.push_back(m);
        }
    }
    imap.Command("LOGOUT", lines, status);

    if (mails.empty()) return L"posta: " + std::to_wstring(uids.size()) + L" nuove (intestazioni non lette), " + tail;
    if (mails.size() <= MaxToasts)
    {
        for (size_t i = 0; i < mails.size(); ++i)
            Toast(mails[i].from.empty() ? L"Posta" : mails[i].from, mails[i].subject.empty() ? L"(senza oggetto)" : mails[i].subject,
                  false, L"mail");
    }
    else
    {
        const NewMail& last = mails.back();
        Toast(std::to_wstring(mails.size()) + L" email nuove", last.from + L": " + last.subject, false, L"mail");
    }
    std::wstring names;
    for (size_t i = 0; i < mails.size(); ++i) names += (i ? L", " : L"") + mails[i].from;
    return L"posta: avviso per " + names + L", " + tail;
}
