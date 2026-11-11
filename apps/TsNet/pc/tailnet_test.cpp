// End-to-end on the PC: join the tailnet as "tsnet-pc-test", reach ponte-pixel through DERP + WireGuard and
// talk to the bridge console (port 8790, forwarded by tailscaled's netstack to 127.0.0.1).
// usage: tailnet_test <statefile> [authkey]
#include "../tailnet.h"
#include <cstdio>
using namespace ts;
int main(int argc, char** argv) {
    Tailnet::Options o;
    o.statePath = argv[1];
    if (argc > 2) o.authKey = argv[2];
    o.hostname = "tsnet-pc-test";
    o.log = [](const std::string& s) { printf("[%lld] %s\n", (long long)(nowMs() % 100000000), s.c_str()); fflush(stdout); };
    o.onAuthUrl = [](const std::string& u) { printf("LOGIN %s\n", u.c_str()); fflush(stdout); };
    Tailnet tn(o);
    tn.start();
    std::string err;
    std::shared_ptr<TcpConn> c = tn.dial("ponte-pixel", 8790, 15 * 60000, err);
    if (!c) { printf("dial: %s\n", err.c_str()); return 1; }
    printf("connected\n");
    int64_t t0 = nowMs();
    c->write(std::string("state\nchats\n"));
    std::string got;
    uint8_t buf[4096];
    while (nowMs() - t0 < 8000) {
        int r = c->read(buf, sizeof buf, 1000);
        if (r == 0) break;
        if (r > 0) got.append((char*)buf, r);
        if (got.find("\n.\n", got.find("\n.\n") + 1) != std::string::npos) break;
    }
    printf("rtt-ish %lld ms, %zu bytes:\n%s\n", (long long)(nowMs() - t0), got.size(), got.substr(0, 600).c_str());
    c->close();
    sleepMs(500);
    tn.stop();
}
