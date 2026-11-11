// Remoto over Tailscale from the PC: join as "tsnet-pc-remoto", dial ponte-pixel on the given ports and report how
// long the TCP open takes and the first bytes (Remoto's 'C' challenge on 8795).
// usage: remoto_tstest <statefile> <port> [port...]
#include "../tailnet.h"
#include <cstdio>
#include <cstdlib>
using namespace ts;
int main(int argc, char** argv) {
    Tailnet::Options o;
    o.statePath = argv[1];
    o.hostname = "tsnet-pc-remoto";
    o.log = [](const std::string& s) { printf("[%lld] %s\n", (long long)(nowMs() % 100000000), s.c_str()); fflush(stdout); };
    o.onAuthUrl = [](const std::string& u) { printf("LOGIN %s\n", u.c_str()); fflush(stdout); };
    Tailnet tn(o);
    tn.start();
    for (int i = 2; i < argc; i++) {
        int port = atoi(argv[i]);
        int64_t t0 = nowMs();
        std::string err;
        std::shared_ptr<TcpConn> c = tn.dial("ponte-pixel", (uint16_t)port, 300000, err);
        if (!c) { printf("port %d: dial failed after %lld ms: %s\n", port, (long long)(nowMs() - t0), err.c_str()); continue; }
        printf("port %d: open in %lld ms\n", port, (long long)(nowMs() - t0));
        uint8_t buf[64];
        int r = c->read(buf, sizeof buf, 5000);
        printf("port %d: read %d bytes, first 0x%02x ('%c') after %lld ms\n", port, r, r > 0 ? buf[0] : 0, r > 0 ? buf[0] : ' ', (long long)(nowMs() - t0));
        fflush(stdout);
        c->close();
    }
    sleepMs(500);
    tn.stop();
}
