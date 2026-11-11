// Parity and accuracy test of apps/Assistente/Assistente/Core/Wsr.cpp on the PC.
// g++ -O2 -std=c++11 -o wsrtest wsrtest.cpp ../../apps/Assistente/Assistente/Core/Wsr.cpp
// ./wsrtest voce.wsr list.tsv     (list: wav path <TAB> transcript, 16 kHz mono 16-bit WAVs)
// ./wsrtest --cep file.wav        (prints c0..c12 and energy of every frame, for the parity check with ear.py)
#include "../../apps/Assistente/Assistente/Core/Wsr.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

static std::vector<int16_t> Wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t p = 12;
    while (p + 8 <= b.size()) {
        std::string id = b.substr(p, 4);
        uint32_t len; memcpy(&len, b.data() + p + 4, 4);
        if (id == "data") {
            std::vector<int16_t> s(len / 2);
            memcpy(s.data(), b.data() + p + 8, s.size() * 2);
            return s;
        }
        p += 8 + len;
    }
    return {};
}

static std::string Norm(const std::string& s) {
    std::string o; bool sp = true;
    for (unsigned char c : s) {
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
        if (c >= 'A' && c <= 'Z') { c = c - 'A' + 'a'; keep = true; }
        if (keep) { o += (char)c; sp = false; } else if (!sp) { o += ' '; sp = true; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--cep") {
        auto pcm = Wav(argv[2]);
        std::vector<float> cep, en;
        assist::WsrCepstra(pcm.data(), pcm.size(), cep, en);
        for (size_t t = 0; t < en.size(); ++t) {
            printf("%.4f", en[t]);
            for (int q = 0; q < 13; ++q) printf(" %.4f", cep[t * 13 + q]);
            printf("\n");
        }
        return 0;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assist::Wsr w;
    std::string err;
    if (!w.Load(b, err)) { std::cerr << "load: " << err << "\n"; return 1; }
    std::ifstream l(argv[2]);
    std::string line;
    int n = 0, ok = 0;
    double ms = 0;
    while (std::getline(l, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        auto pcm = Wav(line.substr(0, tab));
        auto r = w.Decode(pcm.data(), pcm.size());
        bool good = Norm(r.text) == Norm(line.substr(tab + 1));
        ++n; ok += good; ms += r.ms;
        printf("%s gap %.3f  \"%s\" -> \"%s\"\n", good ? "OK" : "  ", r.gap, line.substr(tab + 1).c_str(), r.text.c_str());
    }
    printf("right sentence first: %d/%d; %.0f ms per sentence on this PC; %zu sentences\n", ok, n, ms / std::max(n, 1), w.Sentences());
}
