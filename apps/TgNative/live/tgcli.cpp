// tgcli — PC driver for core/engine: production (or -test) Telegram from the command line.
//   build/tgcli [-test] <statefile>     then commands on stdin (one per line):
//   call <json-ish>   is not supported; use the fixed commands below.
//   config | nearest | code <phone> | signin <code> | password <pw> | dialogs | history <peerline> | send <peerline> <text>
//   wait <sec> | quit
#include "../core/engine.h"
#include "../core/client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace tg;

class PosixHost : public IEngineHost {
public:
    std::map<int, int> fds;   // dc -> fd
    std::map<int, bool> connecting;
    Engine* engine;
    std::string statePath;
    PosixHost() : engine(0) {}
    void netConnect(int conn, const std::string& host, int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        sockaddr_in a; std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, host.c_str(), &a.sin_addr);
        connect(fd, (sockaddr*)&a, sizeof(a));
        fds[conn] = fd; connecting[conn] = true;
    }
    void netSend(int conn, const std::vector<uint8_t>& d) {
        std::size_t off = 0;
        while (off < d.size()) {
            ssize_t n = send(fds[conn], &d[off], d.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno == EAGAIN) { pollfd p = {fds[conn], POLLOUT, 0}; poll(&p, 1, 1000); continue; }
            if (n <= 0) return;
            off += n;
        }
    }
    void netClose(int conn) { if (fds.count(conn)) { close(fds[conn]); fds.erase(conn); connecting.erase(conn); } }
    void randomBytes(uint8_t* out, std::size_t n) { std::ifstream f("/dev/urandom", std::ios::binary); f.read((char*)out, n); }
    int64_t unixTimeMs() { timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000LL + tv.tv_usec / 1000; }
    void onUpdates(const TlValue& u) { std::cout << "UPDATE " << u.dump(4) << std::endl; if (client) client->onUpdates(u); }
    void onStateChanged() { std::ofstream(statePath.c_str()) << engine->save(); }
    void onOnline(bool on) { std::cout << "ONLINE " << on << std::endl; }
    void onAuthLost() { std::cout << "AUTH LOST" << std::endl; if (client) client->onAuthLost(); }
    void onSessionReset() { if (client) client->onSessionReset(); }
    void log(const std::string& l) { std::cout << "log: " << l << std::endl; }
    Client* client = 0;

    void loop(int ms) {
        int64_t end = unixTimeMs() + ms;
        while (unixTimeMs() < end) {
            std::vector<pollfd> ps; std::vector<int> dcs;
            for (std::map<int, int>::iterator it = fds.begin(); it != fds.end(); ++it) {
                pollfd p = {it->second, (short)(connecting[it->first] ? POLLOUT : POLLIN), 0};
                ps.push_back(p); dcs.push_back(it->first);
            }
            poll(ps.empty() ? 0 : &ps[0], ps.size(), 200);
            for (std::size_t i = 0; i < ps.size(); ++i) {
                int dc = dcs[i];
                if (!fds.count(dc)) continue;
                if (connecting[dc] && (ps[i].revents & (POLLOUT | POLLERR | POLLHUP))) {
                    int err = 0; socklen_t l = sizeof(err); getsockopt(fds[dc], SOL_SOCKET, SO_ERROR, &err, &l);
                    connecting[dc] = false;
                    if (err) { engine->onClosed(dc); continue; }
                    engine->onConnected(dc);
                } else if (ps[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    uint8_t buf[65536];
                    ssize_t n = recv(fds[dc], buf, sizeof(buf), 0);
                    if (n <= 0) { netClose(dc); engine->onClosed(dc); continue; }
                    engine->onData(dc, buf, n);
                }
            }
            engine->tick();
        }
    }
};

int main(int argc, char** argv)
{
    bool test = argc > 1 && std::string(argv[1]) == "-test";
    std::string statePath = argc > (test ? 2 : 1) ? argv[test ? 2 : 1] : "build/tg.state";
    std::ifstream sec("../../secrets/telegram.json");
    std::stringstream ss; ss << sec.rdbuf();
    std::string js = ss.str();
    int apiId = std::atoi(js.c_str() + js.find(':', js.find("api_id")) + 1);
    std::size_t h = js.find('"', js.find(':', js.find("api_hash")) + 1);
    std::string apiHash = js.substr(h + 1, js.find('"', h + 1) - h - 1);

    PosixHost host;
    host.statePath = statePath;
    EngineConfig cfg;
    cfg.apiId = apiId; cfg.deviceModel = "Lumia 1020"; cfg.systemVersion = "Windows Phone 8.1"; cfg.appVersion = "0.1"; cfg.testMode = test;
    Engine engine(&host, cfg);
    host.engine = &engine;
    std::ifstream st(statePath.c_str());
    if (st) { std::stringstream b; b << st.rdbuf(); engine.load(b.str()); }
    Client client(&engine, apiId, apiHash);
    host.client = &client;
    std::ifstream cst((statePath + ".client").c_str());
    if (cst) { std::stringstream b; b << cst.rdbuf(); client.load(b.str()); }
    engine.start();
    client.start();

    auto print = [](const char* what) {
        return [what](const TlValue& v, const RpcErr& e) {
            if (e.ok()) std::cout << what << " OK " << v.dump(5) << std::endl;
            else std::cout << what << " ERROR " << e.code << " " << e.message << std::endl;
        };
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd; in >> cmd;
        if (cmd == "quit") break;
        else if (cmd == "wait") { int s = 1; in >> s; host.loop(s * 1000); }
        else if (cmd == "config") engine.invoke(tlObj("help.getConfig"), print("config"));
        else if (cmd == "nearest") engine.invoke(tlObj("help.getNearestDc"), print("nearest"));
        else if (cmd.empty() || cmd[0] == '#') continue;
        else if (cmd == "code") { std::string ph; in >> ph; client.sendCode(ph, [&client](const RpcErr& e) { std::cout << "code: " << e.code << " " << e.message << " via " << client.codeInfo() << std::endl; }); }
        else if (cmd == "signin") { std::string c; in >> c; client.signIn(c, [&client](const RpcErr& e) { std::cout << "signin: " << e.code << " " << e.message << " state " << client.authState() << " hint " << client.passwordHint() << std::endl; }); }
        else if (cmd == "password") { std::string pw; std::getline(in, pw); pw = pw.substr(pw.find_first_not_of(' '));
            client.checkPassword(pw, [&client](const RpcErr& e) { std::cout << "password: " << e.code << " " << e.message << " state " << client.authState() << std::endl; }); }
        else if (cmd == "dialogs") client.loadChats([&client](const RpcErr& e) {
            std::cout << "dialogs: " << e.code << " " << e.message << std::endl;
            for (std::size_t i = 0; i < client.chats().size() && i < 30; ++i) { const ChatItem& c = client.chats()[i];
                std::cout << "  " << c.key << " | " << c.title << " | " << c.unread << " | " << c.lastText.substr(0, 60) << std::endl; } });
        else if (cmd == "history") { std::string k; int n = 10; in >> k >> n; client.loadHistory(k, 0, n, [](const std::vector<MsgItem>& ms, const RpcErr& e) {
            std::cout << "history: " << e.code << " " << e.message << std::endl;
            for (std::size_t i = 0; i < ms.size(); ++i) std::cout << "  " << ms[i].id << " " << ms[i].fromName << " k" << ms[i].kind << " media=" << ms[i].mediaKey << " thumb=" << ms[i].thumbKey << " strip=" << ms[i].strippedJpeg.size() << " dur=" << ms[i].duration << ": " << Client::previewText(ms[i]).substr(0, 80) << std::endl; }); }
        else if (cmd == "send") { std::string k, t; in >> k; std::getline(in, t); client.sendText(k, t.substr(1), 0, [](const MsgItem& m, const RpcErr& e) { std::cout << "sent: " << e.code << " " << e.message << " id " << m.id << std::endl; }); }
        else if (cmd == "download") { std::string k, f; in >> k >> f; client.downloadMedia(k, [](int64_t a, int64_t b) { std::cout << "  " << a << "/" << b << std::endl; },
            [f](const std::string& d, const RpcErr& e) { std::cout << "download: " << e.code << " " << e.message << " " << d.size() << std::endl; std::ofstream(f.c_str(), std::ios::binary) << d; }); }
        else if (cmd == "sendphoto" || cmd == "sendvoice") { std::string k, f; int secs = 0; in >> k >> f >> secs;
            std::ifstream fi(f.c_str(), std::ios::binary); std::stringstream b; b << fi.rdbuf();
            Client::SentCallback cb = [](const MsgItem& m, const RpcErr& e) { std::cout << "sent media: " << e.code << " " << e.message << " id " << m.id << " kind " << m.kind << std::endl; };
            if (cmd == "sendphoto") client.sendPhoto(k, b.str(), "", cb); else client.sendVoice(k, b.str(), secs, "", cb); }
        else std::cout << "unknown command" << std::endl;
        host.loop(300);
        std::ofstream((statePath + ".client").c_str()) << client.save();
    }
    host.loop(500);
    std::ofstream((statePath + ".client").c_str()) << client.save();
    return 0;
}
