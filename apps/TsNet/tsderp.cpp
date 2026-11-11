#include "tsderp.h"

#include <algorithm>
#include <cstring>

namespace ts {

enum { SERVER_KEY = 1, CLIENT_INFO = 2, SERVER_INFO = 3, SEND_PACKET = 4, RECV_PACKET = 5, KEEP_ALIVE = 6,
       NOTE_PREFERRED = 7, PING = 0x12, PONG = 0x13 };

Derp::~Derp() { close(); }

void Derp::close() {
    std::lock_guard<std::mutex> l(closeMu_);
    alive_ = false;
    {
        std::lock_guard<std::mutex> q(qMu_);
        qCv_.notify_all();
    }
    if (tls_) tls_->close();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) writer_.join();
}

// The writer's queue: about a second of a mobile uplink. Past it the oldest packets go (they would only arrive
// late; TCP inside resends what matters).
static const size_t kQueueMax = 256 * 1024;

bool Derp::queueFrame(uint8_t type, const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
    std::string f(5 + an + bn, '\0');
    uint8_t* p = (uint8_t*)&f[0];
    uint32_t n = (uint32_t)(an + bn);
    p[0] = type;
    p[1] = (uint8_t)(n >> 24); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 8); p[4] = (uint8_t)n;
    if (an) memcpy(p + 5, a, an);
    if (bn) memcpy(p + 5 + an, b, bn);
    bool dropped = false;
    {
        std::lock_guard<std::mutex> l(qMu_);
        if (!alive_) return false;
        while (!queue_.empty() && queued_ + f.size() > kQueueMax) {
            queued_ -= queue_.front().size();
            queue_.pop_front();
            dropped = true;
        }
        queued_ += f.size();
        queue_.push_back(std::move(f));
        qCv_.notify_one();
    }
    return !dropped;
}

// Sends what is queued, several frames per TLS record (one record and one system call per packet cost the phone
// more than the packets themselves at video rates).
void Derp::writerLoop() {
    std::string batch;
    while (true) {
        {
            std::unique_lock<std::mutex> l(qMu_);
            qCv_.wait(l, [&] { return !alive_ || !queue_.empty(); });
            if (!alive_) return;
            batch.clear();
            while (!queue_.empty() && batch.size() + queue_.front().size() <= 16000) {
                batch += queue_.front();
                queued_ -= queue_.front().size();
                queue_.pop_front();
            }
            if (batch.empty()) {   // one frame bigger than a record: alone
                batch.swap(queue_.front());
                queued_ -= batch.size();
                queue_.pop_front();
            }
        }
        if (!tls_->write((const uint8_t*)batch.data(), batch.size())) {
            alive_ = false;
            tls_->close();   // the reader ends too
            return;
        }
    }
}

bool Derp::writeFrame(uint8_t type, const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
    std::string f(5 + an + bn, '\0');
    uint8_t* p = (uint8_t*)&f[0];
    uint32_t n = (uint32_t)(an + bn);
    p[0] = type;
    p[1] = (uint8_t)(n >> 24); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 8); p[4] = (uint8_t)n;
    if (an) memcpy(p + 5, a, an);
    if (bn) memcpy(p + 5 + an, b, bn);
    return tls_ && tls_->write(p, f.size());
}

static bool readFrame(Stream& s, uint8_t& type, std::string& body, int timeoutMs) {
    uint8_t h[5];
    if (!s.readFull(h, 5, timeoutMs)) return false;
    uint32_t n = (uint32_t)h[1] << 24 | h[2] << 16 | h[3] << 8 | h[4];
    if (n > (1u << 20)) return false;
    type = h[0];
    body.resize(n);
    return n == 0 || s.readFull((uint8_t*)&body[0], n, 30000);
}

bool Derp::connect(const std::string& host, const uint8_t nodePriv[32], std::string& err) {
    host_ = host;
    tls_ = TlsStream::connect(host, 443, 10000, err);
    if (!tls_) return false;
    std::string req = "GET /derp HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: tailscale-lumia\r\n"
                      "Connection: Upgrade\r\nUpgrade: DERP\r\n\r\n";
    std::string head;
    if (!tls_->write(req) || !readHttpHead(*tls_, head, 10000) || head.compare(0, 12, "HTTP/1.1 101") != 0) {
        err = "DERP " + host + ": " + head.substr(0, head.find('\r'));
        return false;
    }
    uint8_t type;
    std::string body;
    if (!readFrame(*tls_, type, body, 10000) || type != SERVER_KEY || body.size() < 40 ||
        memcmp(body.data(), "DERP\xf0\x9f\x94\x91", 8) != 0) {
        err = "DERP: saluto non valido";
        return false;
    }
    uint8_t serverKey[32], pub[32];
    memcpy(serverKey, body.data() + 8, 32);
    x25519Base(pub, nodePriv);
    std::string info = "{\"version\":2,\"CanAckPings\":true}";
    Bytes box = boxSeal(serverKey, nodePriv, (const uint8_t*)info.data(), info.size());
    if (!writeFrame(CLIENT_INFO, pub, 32, box.data(), box.size())) { err = "DERP: scrittura"; return false; }
    if (!readFrame(*tls_, type, body, 10000) || type != SERVER_INFO) { err = "DERP: niente server info"; return false; }
    Bytes plain;
    if (!boxOpen(serverKey, nodePriv, (const uint8_t*)body.data(), body.size(), plain)) {
        err = "DERP: server info non valida";
        return false;
    }
    uint8_t one = 1;
    writeFrame(NOTE_PREFERRED, &one, 1);
    alive_ = true;
    reader_ = std::thread(&Derp::readerLoop, this);
    writer_ = std::thread(&Derp::writerLoop, this);
    return true;
}

bool Derp::send(const uint8_t dst[32], const uint8_t* p, size_t n) {
    if (!alive_) return false;
    return queueFrame(SEND_PACKET, dst, 32, p, n);
}

void Derp::readerLoop() {
    std::string body;
    int64_t budgetUs = 0;   // bench only: when the emulated slow downlink may deliver the next byte
    while (alive_) {
        uint8_t type;
        if (!readFrame(*tls_, type, body, 150000)) break;   // the server sends a keepalive every minute
        if (bench.derpReadBps != 0) {   // < 0: stalled (a tunnel, a handover) until the bench says otherwise
            while (bench.derpReadBps < 0 && alive_) sleepMs(10);
            int rate = bench.derpReadBps;
            int64_t now = nowMs() * 1000;
            if (budgetUs < now) budgetUs = now;
            if (rate > 0) budgetUs += (int64_t)(body.size() + 5) * 1000000 / rate;
            while (alive_ && bench.derpReadBps > 0 && (now = nowMs() * 1000) < budgetUs)
                sleepMs((int)std::min<int64_t>(20, (budgetUs - now) / 1000 + 1));
        }
        if (type == RECV_PACKET && body.size() >= 32 && bench.lossPerMille > 0) {
            uint8_t r[2];
            randomBytes(r, 2);
            if ((r[0] << 8 | r[1]) % 1000 < (unsigned)bench.lossPerMille) continue;
        }
        if (type == RECV_PACKET && body.size() >= 32) {
            onRecv_((const uint8_t*)body.data(), (const uint8_t*)body.data() + 32, body.size() - 32);
        } else if (type == PING && body.size() == 8) {
            queueFrame(PONG, (const uint8_t*)body.data(), 8);
        }
    }
    alive_ = false;
    {
        std::lock_guard<std::mutex> q(qMu_);
        qCv_.notify_all();
    }
    log_("DERP " + host_ + ": chiuso");
}

}  // namespace ts
