#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "TvRemote.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "tsio.h"
#include "tscrypto.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tv {

std::string base64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

namespace {
// value of "key":"..." in a small JSON text ("" if absent)
std::string jsonString(const std::string& s, const std::string& key)
{
    std::string k = "\"" + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return "";
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = s.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return "";
    if (s[p] != '"') {   // a number
        size_t e = s.find_first_of(",}", p);
        return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }
    size_t e = s.find('"', p + 1);
    return e == std::string::npos ? "" : s.substr(p + 1, e - p - 1);
}
}

Remote::Remote() {}
Remote::~Remote() { close(); }

std::string Remote::connect(const std::string& host, const std::string& name, std::string& token, int timeoutMs)
{
    close();
    std::string err;
    std::unique_ptr<ts::TlsStream> made = ts::TlsStream::connect(host, 8002, timeoutMs < 8000 ? timeoutMs : 8000, err);
    std::shared_ptr<ts::TlsStream> s(made.release());
    if (!s) return err.empty() ? "TV non raggiungibile" : err;
    if (log) log("step: tls ok");
    uint8_t nonce[16];
    ts::randomBytes(nonce, sizeof nonce);
    std::string path = "/api/v2/channels/samsung.remote.control?name=" + base64(name);
    if (!token.empty()) path += "&token=" + token;
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":8002\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: " + base64(std::string((const char*)nonce, 16)) +
        "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!s->write(req)) return "invio della richiesta non riuscito";
    std::string head;
    if (log) log("step: upgrade sent");
    if (!ts::readHttpHead(*s, head, 8000)) return "la TV non ha risposto";
    if (log) log("step: head " + head.substr(0, head.find('\r')));
    if (head.find(" 101 ") == std::string::npos) return "la TV ha rifiutato: " + head.substr(0, head.find('\r'));
    {
        std::lock_guard<std::mutex> l(mu_);
        stream_ = s;
    }
    // the first message says whether we are in: immediately with a valid token, after "Allow" otherwise
    int64_t end = ts::nowMs() + timeoutMs;
    while (true) {
        int left = (int)(end - ts::nowMs());
        if (left <= 0) { close(); return "nessuna risposta: hai accettato sulla TV?"; }
        std::string msg;
        int op = readFrame(msg, left);
        if (log) log("step: frame op " + std::to_string((long long)op) + " len " + std::to_string((long long)msg.size()));
        if (op == 0) { close(); return "la TV ha chiuso la connessione (rifiutato?)"; }
        if (op < 0) continue;
        if (op != 1) continue;
        std::string ev = jsonString(msg, "event");
        if (log) log("tv: event " + ev);
        if (ev == "ms.channel.connect") {
            std::string t = jsonString(msg, "token");
            if (log) log(t.empty() ? "step: no token in the answer" : "step: token received");
            if (!t.empty()) token = t;
            return "";
        }
        if (ev == "ms.channel.unauthorized" || ev == "ms.channel.timeOut") {
            close();
            return ev == "ms.channel.timeOut" ? "tempo scaduto: accetta sulla TV" : "la TV non ha autorizzato il telefono";
        }
    }
}

bool Remote::sendKey(const std::string& key, const char* cmd)
{
    return sendText(std::string("{\"method\":\"ms.remote.control\",\"params\":{\"Cmd\":\"") + cmd +
                    "\",\"DataOfCmd\":\"" + key + "\",\"Option\":\"false\",\"TypeOfRemote\":\"SendRemoteKey\"}}");
}

std::shared_ptr<ts::TlsStream> Remote::current() const
{
    std::lock_guard<std::mutex> l(mu_);
    return stream_;
}

bool Remote::connected() const { return current() != nullptr; }

bool Remote::poll(int timeoutMs)
{
    if (!current()) return false;
    std::string msg;
    int op = readFrame(msg, timeoutMs);
    if (op == 0) { close(); return false; }
    if (op == 9) sendFrame(10, msg);   // ping -> pong
    if (op == 8) { close(); return false; }
    return true;
}

void Remote::close()
{
    std::shared_ptr<ts::TlsStream> s;
    {
        std::lock_guard<std::mutex> l(mu_);
        s.swap(stream_);
    }
    if (s) s->close();   // unblocks a read in progress; the last user frees it
}

bool Remote::sendText(const std::string& text) { return sendFrame(1, text); }

bool Remote::sendFrame(int opcode, const std::string& payload)
{
    std::lock_guard<std::mutex> l(sendMu_);
    std::shared_ptr<ts::TlsStream> s = current();
    if (!s) return false;
    std::string f;
    f += (char)(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) f += (char)(0x80 | n);
    else { f += (char)(0x80 | 126); f += (char)(n >> 8); f += (char)(n & 255); }
    uint8_t mask[4];
    ts::randomBytes(mask, 4);
    f.append((const char*)mask, 4);
    for (size_t i = 0; i < n; ++i) f += (char)(payload[i] ^ mask[i & 3]);
    bool ok = s->write(f);
    if (!ok) close();
    return ok;
}

int Remote::readFrame(std::string& payload, int timeoutMs)
{
    std::shared_ptr<ts::TlsStream> s = current();
    if (!s) return 0;
    uint8_t h[2];
    int r = s->read(h, 1, timeoutMs);
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (!s->readFull(h + 1, 1, 5000)) return 0;
    int op = h[0] & 15;
    uint64_t n = h[1] & 127;
    uint8_t ext[8];
    if (n == 126) { if (!s->readFull(ext, 2, 5000)) return 0; n = (ext[0] << 8) | ext[1]; }
    else if (n == 127) {
        if (!s->readFull(ext, 8, 5000)) return 0;
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | ext[i];
    }
    if (n > (1 << 20)) return 0;
    uint8_t mask[4] = {0, 0, 0, 0};
    bool masked = (h[1] & 0x80) != 0;
    if (masked && !s->readFull(mask, 4, 5000)) return 0;
    payload.resize((size_t)n);
    if (n && !s->readFull((uint8_t*)&payload[0], (size_t)n, 5000)) return 0;
    if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];
    return op ? op : 1;   // continuation frames are not expected from the TV: treat as text
}

// dotted IPv4 -> network order (WP8.1's Winsock has no inet_pton); 0 if not an address
static uint32_t ipv4(const std::string& s)
{
    unsigned a, b, c, d;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return htonl((a << 24) | (b << 16) | (c << 8) | d);
}

std::string wakeOnLan(const std::string& mac, const std::string& host)
{
    unsigned b[6];
    if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return "indirizzo MAC non valido";
    std::string pkt(6, '\xff');
    for (int i = 0; i < 16; ++i) for (int j = 0; j < 6; ++j) pkt += (char)b[j];
#ifdef _WIN32
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "socket UDP non disponibile";
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return "socket UDP non disponibile";
#endif
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
    std::vector<std::string> targets;
    targets.push_back("255.255.255.255");
    size_t dot = host.rfind('.');
    if (dot != std::string::npos) targets.push_back(host.substr(0, dot) + ".255");
    targets.push_back(host);
    int sent = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        a.sin_addr.s_addr = ipv4(targets[i]);
        for (int k = 0; k < 3; ++k)
            if (sendto(s, pkt.data(), (int)pkt.size(), 0, (sockaddr*)&a, sizeof a) > 0) ++sent;
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
    return sent ? "" : "pacchetto di accensione non inviato";
}

}  // namespace tv
