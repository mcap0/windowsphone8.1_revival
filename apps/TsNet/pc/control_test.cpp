// Control smoke test: Noise handshake + POST /machine/register with fresh keys (no auth): expects an AuthURL.
#include "../tscontrol.h"
#include <cstdio>
using namespace ts;
namespace ts { extern std::string lastUnknownStatus; }
int main() {
    uint8_t mpriv[32], mpub[32], npriv[32], npub[32];
    newKeyPair(mpriv, mpub);
    newKeyPair(npriv, npub);
    Control c([](const std::string& s) { printf("log: %s\n", s.c_str()); });
    std::string err;
    if (!c.connect(mpriv, err)) { printf("connect: %s\n", err.c_str()); return 1; }
    std::string req = "{\"Version\":148,\"NodeKey\":\"nodekey:" + hex(npub, 32) + "\",\"Hostinfo\":{\"Hostname\":\"tsnet-test\",\"OS\":\"windows\"}}";
    unsigned id = c.post("/machine/register", req);
    printf("status %d\n", c.status(id, 15000));
    printf("unk %s\n", lastUnknownStatus.c_str());
    printf("body %s\n", c.readAll(id, 15000).c_str());
}
