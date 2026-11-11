// PC check of Core/TvRemote against the real TV: remote_test <tv-ip> <tokenfile> KEY_... [KEY_...]
// build: see apps/Telecomando/pc/build.sh
#include "../Telecomando/Core/TvRemote.h"
#include "tsio.h"
#include <cstdio>
#include <fstream>
#include <iostream>
int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: remote_test <tv-ip> <tokenfile> [KEY ...|wol <mac>]\n"); return 2; }
    std::string token;
    { std::ifstream f(argv[2]); std::getline(f, token); }
    if (argc > 4 && std::string(argv[3]) == "wol") {
        std::string e = tv::wakeOnLan(argv[4], argv[1]);
        std::printf("wol: %s\n", e.empty() ? "sent" : e.c_str());
        return e.empty() ? 0 : 1;
    }
    tv::Remote r;
    r.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    int64_t t0 = ts::nowMs();
    std::string e = r.connect(argv[1], "Lumia", token, 30000);
    std::printf("connect %lld ms: %s\n", (long long)(ts::nowMs() - t0), e.empty() ? "ok" : e.c_str());
    if (!e.empty()) return 1;
    { std::ofstream f(argv[2]); f << token << "\n"; }
    for (int i = 3; i < argc; ++i) {
        std::printf("%s %s\n", argv[i], r.sendKey(argv[i]) ? "sent" : "FAILED");
        r.poll(400);
    }
    std::printf("still connected: %s\n", r.poll(200) ? "yes" : "no");
    return 0;
}
