// remotonet: the Lumia's Remoto client on the PC, through our own Tailscale code (DERP only, as from 4G), with a
// slow downlink emulated by reading the DERP stream at a fixed rate (the relay's buffers fill as for the phone).
// Measures what the user sees: frames per second, how old each frame is when it arrives, the longest freeze, and
// for taps the time until the first and the last frame of the reaction. PLAN §65, tools/remotonet runs it.
//
// usage: remotonet --state FILE --key HEX [--port 8796] [--seconds 40] [--rate KBIT] [--rcvbuf KB] [--old]
//                  [--audio] [--tap MS] [--tapx X] [--tapy Y] [--out FILE.h264] [--csv FILE] [--lan IP]
//   --old   the Lumia 1.3.2.5 logic: acks every 30 ms or when drained, catch-up on 3 frames > 250 ms late
//   --punch hole punching (STUN + CallMeMaybe), UDP on but private addresses blocked: only a public path counts
//   --lan   plain TCP to IP:port instead of Tailscale (no shaping)
#include "../tailnet.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/md.h"

using namespace ts;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::vector<uint8_t> hmac(const std::vector<uint8_t>& key, const uint8_t* d, size_t n) {
    std::vector<uint8_t> out(32);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key.data(), key.size(), d, n, out.data());
    return out;
}

// One connection: Tailscale TcpConn or a plain socket.
static std::atomic<int64_t> g_deadline(0);   // reads give up after it (a stalled stream must still end the test)

struct Link {
    std::shared_ptr<TcpConn> tcp;
    int fd = -1;
    std::mutex sendMu;
    std::vector<uint8_t> buf;
    size_t off = 0;
    bool fill() {
        if (off == buf.size()) { buf.clear(); off = 0; }
        size_t at = buf.size();
        buf.resize(at + 65536);
        int n;
        if (tcp) {
            while ((n = tcp->read(buf.data() + at, 65536, 500)) < 0)
                if (g_deadline && nowUs() > g_deadline + 5000000) { n = 0; break; }
        }
        else n = (int)recv(fd, buf.data() + at, 65536, 0);
        buf.resize(at + (n > 0 ? n : 0));
        return n > 0;
    }
    bool get(void* p, size_t n) {
        uint8_t* q = (uint8_t*)p;
        while (n) {
            if (off == buf.size() && !fill()) return false;
            size_t k = std::min(n, buf.size() - off);
            memcpy(q, buf.data() + off, k);
            off += k; q += k; n -= k;
        }
        return true;
    }
    size_t queued() { return buf.size() - off + (tcp ? tcp->queued() : 0); }
    void send(char kind, const void* p, size_t n) {
        std::string m(1, kind);
        if (n) m.append((const char*)p, n);
        std::lock_guard<std::mutex> l(sendMu);
        if (tcp) tcp->write((const uint8_t*)m.data(), m.size());
        else ::send(fd, m.data(), m.size(), MSG_NOSIGNAL);
    }
    void block(char kind, const std::string& s) {
        std::string b(4, '\0');
        uint32_t n = (uint32_t)s.size();
        memcpy(&b[0], &n, 4);
        send(kind, (b + s).data(), 4 + s.size());
    }
};

struct Frame { uint32_t seq, size; bool key; int64_t server, arrival, owd; };

static std::string pct(std::vector<int64_t> v) {
    if (v.empty()) return "-";
    std::sort(v.begin(), v.end());
    char b[96];
    snprintf(b, sizeof b, "p50 %lld p90 %lld p99 %lld max %lld", (long long)v[v.size() / 2] / 1000,
             (long long)v[v.size() * 9 / 10] / 1000, (long long)v[std::min(v.size() - 1, v.size() * 99 / 100)] / 1000,
             (long long)v.back() / 1000);
    return b;
}

int main(int argc, char** argv) {
    std::string state = "remotonet.state", keyHex, out, csv, lan, profile;
    int port = 8796, seconds = 40, rateKbit = 0, rcvKB = 0, tapMs = 0, tapX = 600, tapY = 640;
    bool old = false, audio = false, punch = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--state") state = next();
        else if (a == "--key") keyHex = next();
        else if (a == "--port") port = atoi(next().c_str());
        else if (a == "--seconds") seconds = atoi(next().c_str());
        else if (a == "--rate") rateKbit = atoi(next().c_str());
        else if (a == "--rcvbuf") rcvKB = atoi(next().c_str());
        else if (a == "--old") old = true;
        else if (a == "--audio") audio = true;
        else if (a == "--tap") tapMs = atoi(next().c_str());
        else if (a == "--tapx") tapX = atoi(next().c_str());
        else if (a == "--tapy") tapY = atoi(next().c_str());
        else if (a == "--out") out = next();
        else if (a == "--csv") csv = next();
        else if (a == "--lan") lan = next();
        else if (a == "--punch") punch = true;   // hole punching, LAN addresses blocked (only the public path)
        else if (a == "--loss") bench.lossPerMille = atoi(next().c_str());   // per mille of downlink packets lost
        else if (a == "--profile") profile = next();   // "secs:kbit,secs:kbit,...": the downlink over time (0 = stalled)
    }
    std::vector<uint8_t> secret;
    for (size_t i = 0; i + 1 < keyHex.size(); i += 2) secret.push_back((uint8_t)strtoul(keyHex.substr(i, 2).c_str(), nullptr, 16));
    std::vector<uint8_t> macKey = hmac(secret, (const uint8_t*)"mac", 3);

    bench.noUdp = !punch;
    bench.noLan = punch;
    bench.derpReadBps = rateKbit * 1000 / 8;
    bench.rcvBuf = rcvKB * 1024;
    Link L;
    std::unique_ptr<Tailnet> tn;
    int64_t t0 = nowUs();
    if (!lan.empty()) {
        L.fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, lan.c_str(), &a.sin_addr);
        int one = 1;
        setsockopt(L.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(L.fd, (sockaddr*)&a, sizeof a) != 0) { printf("connect failed\n"); return 1; }
    } else {
        Tailnet::Options o;
        o.statePath = state;
        o.hostname = "tsnet-pc-remoto";
        o.holePunch = punch;
        o.log = [](const std::string& s) { fprintf(stderr, "ts: %s\n", s.c_str()); };
        o.onAuthUrl = [](const std::string& u) { printf("LOGIN %s\n", u.c_str()); fflush(stdout); };
        tn.reset(new Tailnet(o));
        tn->start();
        std::string err;
        L.tcp = tn->dial("ponte-pixel", (uint16_t)port, 60000, err);
        if (!L.tcp) { printf("dial: %s\n", err.c_str()); return 1; }
    }
    printf("connected in %lld ms (%s, rate %d kbit/s, rcvbuf %d KB, %s)\n", (long long)(nowUs() - t0) / 1000,
           lan.empty() ? "DERP" : "LAN", rateKbit, rcvKB, old ? "old client logic" : "new client logic");
    fflush(stdout);

    std::atomic<bool> stop(false);
    std::atomic<int64_t> offset(0);
    std::atomic<bool> offsetKnown(false);
    int64_t bestRtt = INT64_MAX;
    std::vector<int64_t> rtts;
    std::mutex tapMu;
    std::vector<int64_t> taps;   // client time of each tap
    FILE* h264 = out.empty() ? nullptr : fopen(out.c_str(), "wb");
    FILE* fcsv = csv.empty() ? nullptr : fopen(csv.c_str(), "w");
    if (fcsv) fprintf(fcsv, "seq,size,key,server_us,arrival_us,owd_us\n");

    std::thread pinger, tapper, profiler;
    std::vector<Frame> frames;
    int64_t audioPackets = 0, audioBytes = 0, keys = 0, resets = 0;
    bool authed = false, catching = false;
    int lateFrames = 0;
    int64_t catchStart = 0, keyAsked = 0, ackAt = 0;
    uint32_t ackSeq = 0;
    bool ackPending = false;
    int64_t end = 0;
    std::vector<uint8_t> unit;
    while (true) {
        if (old && ackPending && (nowUs() - ackAt > 30000 || L.queued() == 0)) {
            ackAt = nowUs();
            ackPending = false;
            L.send('F', &ackSeq, 4);
        }
        if (end && nowUs() > end) break;
        char kind;
        if (!L.get(&kind, 1)) { printf("connection closed\n"); break; }
        if (kind == 'C') {
            uint8_t nonce[16];
            if (!L.get(nonce, 16)) break;
            L.block('S', std::string("banco remotonet: ") + (lan.empty() ? "DERP" : "LAN") + " " + std::to_string(rateKbit) + " kbit/s");
            std::vector<uint8_t> ans = hmac(macKey, nonce, 16);
            if (audio) L.send('O', nullptr, 0);
            uint8_t r0 = 0;
            L.send('W', &r0, 1);
            L.send('A', ans.data(), ans.size());
            authed = true;
            end = nowUs() + (int64_t)seconds * 1000000;
            g_deadline = end;
            pinger = std::thread([&]() {
                while (!stop) { int64_t n = nowUs(); L.send('p', &n, 8); usleep(500000); }
            });
            if (!profile.empty())
                profiler = std::thread([&, profile]() {
                    int64_t start = nowUs();
                    size_t p = 0;
                    while (!stop && p < profile.size()) {
                        size_t comma = profile.find(',', p);
                        std::string step = profile.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
                        p = comma == std::string::npos ? profile.size() : comma + 1;
                        int at = atoi(step.c_str()), kbit = atoi(step.c_str() + step.find(':') + 1);
                        while (!stop && nowUs() - start < (int64_t)at * 1000000) usleep(20000);
                        bench.derpReadBps = kbit > 0 ? kbit * 1000 / 8 : -1;
                        printf("[%5.1f s] downlink %d kbit/s\n", (nowUs() - start) / 1e6, kbit);
                        fflush(stdout);
                    }
                });
            if (tapMs > 0)
                tapper = std::thread([&]() {
                    usleep(3000000);
                    while (!stop) {
                        uint8_t b[6] = {0, 0};
                        uint16_t x = (uint16_t)tapX, y = (uint16_t)tapY;
                        memcpy(b + 2, &x, 2); memcpy(b + 4, &y, 2);
                        { std::lock_guard<std::mutex> l(tapMu); taps.push_back(nowUs()); }
                        L.send('T', b, 6);
                        usleep(40000);
                        b[0] = 1;
                        L.send('T', b, 6);
                        for (int i = 0; i < tapMs / 10 && !stop; i++) usleep(10000);
                    }
                });
            continue;
        }
        if (kind == 'P') {
            uint8_t b[16];
            if (!L.get(b, 16)) break;
            int64_t now = nowUs(), server, sent;
            memcpy(&server, b, 8); memcpy(&sent, b + 8, 8);
            int64_t rtt = now - sent;
            rtts.push_back(rtt);
            bestRtt = bestRtt == INT64_MAX ? rtt : bestRtt + 200;
            if (rtt <= bestRtt) { bestRtt = rtt; offset = server - (sent + rtt / 2); offsetKnown = true; }
            continue;
        }
        if (kind == 'U') {
            uint8_t h[12];
            if (!L.get(h, 12)) break;
            uint32_t n; memcpy(&n, h, 4);
            std::vector<uint8_t> p(n);
            if (n && !L.get(p.data(), n)) break;
            audioPackets++; audioBytes += n + 13;
            continue;
        }
        if (kind == 'k' || kind == 'o' || kind == 'a') {
            uint32_t n;
            if (!L.get(&n, 4)) break;
            std::string j(n, '\0');
            if (n && !L.get(&j[0], n)) break;
            continue;
        }
        if (kind != 'V') { printf("unknown message %d\n", kind); break; }
        uint8_t h[16];
        if (!L.get(h, 16)) break;
        uint32_t size, seq; int64_t server;
        memcpy(&size, h, 4); memcpy(&server, h + 4, 8); memcpy(&seq, h + 12, 4);
        unit.resize(size);
        if (!L.get(unit.data(), size)) break;
        int64_t arrival = nowUs();
        if (!old) L.send('F', &seq, 4);   // every frame at once: the Pixel paces on these
        else { ackSeq = seq; ackPending = true; }
        bool key = size > 4 && (unit[4] & 0x1F) == 7;
        if (key) keys++;
        int64_t owd = offsetKnown ? arrival + offset - server : 0;
        frames.push_back(Frame{seq, size, key, server, arrival, owd});
        if (fcsv) fprintf(fcsv, "%u,%u,%d,%lld,%lld,%lld\n", seq, size, key ? 1 : 0, (long long)server, (long long)(arrival - t0), (long long)owd);
        if (old) {   // Lumia 1.3.2.5 catch-up
            if (!catching) {
                lateFrames = owd > 250000 ? lateFrames + 1 : 0;
                if (lateFrames >= 3) { catching = true; catchStart = keyAsked = arrival; lateFrames = 0; resets++; L.send('K', nullptr, 0); }
            }
            if (catching) {
                bool usable = key && (owd < 100000 || L.queued() == 0 || arrival - catchStart > 1000000);
                if (!usable) {
                    if (arrival - keyAsked > 300000) { keyAsked = arrival; L.send('K', nullptr, 0); }
                    continue;
                }
                catching = false;
            }
        }
        if (h264) fwrite(unit.data(), 1, size, h264);
    }
    stop = true;
    if (pinger.joinable()) pinger.join();
    if (L.tcp) { L.tcp->close(); }
    if (tapper.joinable()) tapper.join();
    if (profiler.joinable()) profiler.join();
    if (h264) fclose(h264);
    if (fcsv) fclose(fcsv);
    if (!authed || frames.empty()) { printf("no frames\n"); return 1; }

    // Summary
    int64_t first = frames.front().arrival, last = frames.back().arrival;
    double secs = std::max(1e-3, (last - first) / 1e6);
    int64_t bytes = 0, maxGap = 0;
    std::vector<int64_t> owds, gaps;
    for (size_t i = 0; i < frames.size(); i++) {
        bytes += frames[i].size + 17;
        if (frames[i].owd) owds.push_back(frames[i].owd);
        if (i) { int64_t g = frames[i].arrival - frames[i - 1].arrival; gaps.push_back(g); maxGap = std::max(maxGap, g); }
    }
    printf("frames %zu in %.1f s = %.1f fps, video %.0f kbit/s, audio %lld packets %.0f kbit/s, key frames %lld, resets %lld\n",
           frames.size(), secs, frames.size() / secs, bytes * 8 / secs / 1000, (long long)audioPackets,
           audioBytes * 8 / secs / 1000, (long long)keys, (long long)resets);
    printf("frame age on arrival (ms): %s\n", pct(owds).c_str());
    printf("gaps between frames (ms): %s\n", pct(gaps).c_str());
    printf("ping rtt (ms): %s\n", pct(rtts).c_str());
    // taps: first frame produced after the tap was sent (server clock), and the end of its burst (no frame for 300 ms)
    std::vector<int64_t> firstLat, doneLat;
    for (size_t t = 0; t < taps.size(); t++) {
        int64_t tap = taps[t], tapServer = tap + offset;
        size_t i = 0;
        while (i < frames.size() && frames[i].server < tapServer) i++;
        if (i == frames.size()) continue;
        firstLat.push_back(frames[i].arrival - tap);
        size_t j = i;
        while (j + 1 < frames.size() && frames[j + 1].server - frames[j].server < 300000 &&
               (t + 1 >= taps.size() || frames[j + 1].server < taps[t + 1] + offset)) j++;
        doneLat.push_back(frames[j].arrival - tap);
    }
    if (!taps.empty()) {
        printf("taps %zu: tap -> first new frame (ms): %s\n", taps.size(), pct(firstLat).c_str());
        printf("        tap -> last frame of the reaction (ms): %s\n", pct(doneLat).c_str());
    }
    if (L.tcp) { printf("tcp: %s\n", L.tcp->stats().c_str()); L.tcp->close(); sleepMs(500); }   // FIN before the tailnet goes
    if (tn) tn->stop();
    return 0;
}
