#include "tswg.h"

#include <chrono>
#include <cstring>

#include "tsio.h"

namespace ts {

static const char* kConstruction = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char* kIdentifier = "WireGuard v1 zx2c4 Jason@zx2c4.com";

static inline void put32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t get32le(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t get64le(const uint8_t* p) { return (uint64_t)get32le(p) | (uint64_t)get32le(p + 4) << 32; }
static void nonceLE(uint8_t n[12], uint64_t c) {
    memset(n, 0, 4);
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(c >> (8 * i));
}

static void initChain(uint8_t ck[32], uint8_t h[32]) {
    blake2s(ck, kConstruction, strlen(kConstruction));
    blake2s(h, ck, 32, kIdentifier, strlen(kIdentifier));
}

static void mixHash(uint8_t h[32], const uint8_t* p, size_t n) { blake2s(h, h, 32, p, n); }

static bool mixDH(uint8_t ck[32], uint8_t* k, const uint8_t priv[32], const uint8_t pub[32]) {
    uint8_t dh[32];
    if (!x25519(dh, priv, pub)) return false;
    if (k) kdf(ck, dh, 32, ck, k);
    else kdf(ck, dh, 32, ck);
    return true;
}

static uint32_t randomIndex() {
    uint8_t b[4];
    randomBytes(b, 4);
    return get32le(b);
}

WgPeer::WgPeer(const uint8_t priv[32], const uint8_t peerPub[32], SendFn send, IpFn onIp) : send_(send), onIp_(onIp) {
    memcpy(priv_, priv, 32);
    x25519Base(pub_, priv);
    memcpy(peer_, peerPub, 32);
}

bool WgPeer::up() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    return cur_.valid && cur_.confirmed && nowMs() - cur_.born < 180000;
}

void WgPeer::mac1(uint8_t* msg, size_t macOff) {
    uint8_t key[32];
    blake2s(key, "mac1----", 8, peer_, 32);
    Blake2s m(16, key, 32);
    m.update(msg, macOff);
    m.final(msg + macOff);
    memset(msg + macOff + 16, 0, 16);   // mac2: no cookie
}

void WgPeer::startHandshake() {
    int64_t now = nowMs();
    if (hsPending_ && now - hsSent_ < 5000) return;
    uint8_t msg[148] = {0}, ck[32], h[32], k[32], ePub[32];
    initChain(ck, h);
    mixHash(h, peer_, 32);
    newKeyPair(hsEPriv_, ePub);
    msg[0] = 1;
    hsIdx_ = randomIndex();
    put32le(msg + 4, hsIdx_);
    memcpy(msg + 8, ePub, 32);
    kdf(ck, ePub, 32, ck);
    mixHash(h, ePub, 32);
    if (!mixDH(ck, k, hsEPriv_, peer_)) return;
    uint8_t zero[12] = {0};
    aeadSeal(k, zero, pub_, 32, h, 32, msg + 40);
    mixHash(h, msg + 40, 48);
    if (!mixDH(ck, k, priv_, peer_)) return;
    // TAI64N, strictly increasing for this peer
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (ns <= lastTs_) ns = lastTs_ + 1;
    lastTs_ = ns;
    uint64_t secs = 0x400000000000000aULL + (uint64_t)(ns / 1000000000);
    uint32_t nano = (uint32_t)(ns % 1000000000);
    uint8_t tai[12];
    for (int i = 0; i < 8; i++) tai[i] = (uint8_t)(secs >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) tai[8 + i] = (uint8_t)(nano >> (24 - 8 * i));
    aeadSeal(k, zero, tai, 12, h, 32, msg + 88);
    mixHash(h, msg + 88, 28);
    memcpy(hsCk_, ck, 32);
    memcpy(hsH_, h, 32);
    mac1(msg, 116);
    hsPending_ = true;
    hsSent_ = lastHsAttempt_ = now;
    send_(msg, sizeof msg);
}

bool WgPeer::handleResponse(const uint8_t* p, size_t n) {
    if (n != 92 || !hsPending_ || get32le(p + 8) != hsIdx_) return false;
    uint8_t ck[32], h[32], k[32], t[32];
    memcpy(ck, hsCk_, 32);
    memcpy(h, hsH_, 32);
    const uint8_t* rePub = p + 12;
    kdf(ck, rePub, 32, ck);
    mixHash(h, rePub, 32);
    if (!mixDH(ck, nullptr, hsEPriv_, rePub) || !mixDH(ck, nullptr, priv_, rePub)) return false;
    uint8_t psk[32] = {0};
    kdf(ck, psk, 32, ck, t, k);
    mixHash(h, t, 32);
    uint8_t zero[12] = {0}, empty[1];
    if (!aeadOpen(k, zero, p + 44, 16, h, 32, empty)) return false;
    hsPending_ = false;
    prev_ = cur_;
    cur_ = Session();
    cur_.valid = cur_.confirmed = cur_.initiator = true;
    cur_.localIdx = hsIdx_;
    cur_.remoteIdx = get32le(p + 4);
    kdf(ck, nullptr, 0, cur_.sendKey, cur_.recvKey);
    memset(cur_.window, 0, sizeof cur_.window);
    cur_.born = nowMs();
    flushQueue();
    if (queue_.empty()) encryptAndSend(cur_, nullptr, 0);   // keepalive confirms the session to the responder
    return true;
}

bool WgPeer::handleInitiation(const uint8_t* p, size_t n) {
    if (n != 148) return false;
    uint8_t ck[32], h[32], k[32];
    initChain(ck, h);
    mixHash(h, pub_, 32);
    const uint8_t* eiPub = p + 8;
    kdf(ck, eiPub, 32, ck);
    mixHash(h, eiPub, 32);
    if (!mixDH(ck, k, priv_, eiPub)) return false;
    uint8_t zero[12] = {0}, peerStatic[32], tai[12];
    if (!aeadOpen(k, zero, p + 40, 48, h, 32, peerStatic) || memcmp(peerStatic, peer_, 32) != 0) return false;
    mixHash(h, p + 40, 48);
    if (!mixDH(ck, k, priv_, peer_)) return false;
    if (!aeadOpen(k, zero, p + 88, 28, h, 32, tai)) return false;
    mixHash(h, p + 88, 28);

    uint8_t msg[92] = {0}, ePriv[32], ePub[32], t[32];
    newKeyPair(ePriv, ePub);
    msg[0] = 2;
    Session s;
    s.localIdx = randomIndex();
    s.remoteIdx = get32le(p + 4);
    put32le(msg + 4, s.localIdx);
    put32le(msg + 8, s.remoteIdx);
    memcpy(msg + 12, ePub, 32);
    kdf(ck, ePub, 32, ck);
    mixHash(h, ePub, 32);
    if (!mixDH(ck, nullptr, ePriv, eiPub) || !mixDH(ck, nullptr, ePriv, peer_)) return false;
    uint8_t psk[32] = {0};
    kdf(ck, psk, 32, ck, t, k);
    mixHash(h, t, 32);
    aeadSeal(k, zero, nullptr, 0, h, 32, msg + 44);
    mac1(msg, 60);
    s.valid = true;
    kdf(ck, nullptr, 0, s.recvKey, s.sendKey);
    memset(s.window, 0, sizeof s.window);
    s.born = nowMs();
    next_ = s;
    send_(msg, sizeof msg);
    return true;
}

bool WgPeer::replayOk(Session& s, uint64_t ctr) {
    if (ctr + 1024 <= s.recvMax && s.recvMax >= 1024) return false;
    if (ctr > s.recvMax) {
        uint64_t shift = ctr - s.recvMax;
        if (shift >= 1024) memset(s.window, 0, sizeof s.window);
        else
            for (uint64_t c = s.recvMax + 1; c <= ctr; c++) s.window[(c / 64) % 16] &= ~(1ULL << (c % 64));
        s.recvMax = ctr;
    }
    uint64_t& w = s.window[(ctr / 64) % 16];
    uint64_t bit = 1ULL << (ctr % 64);
    if (w & bit) return false;
    w |= bit;
    return true;
}

bool WgPeer::handleData(const uint8_t* p, size_t n) {
    if (n < 32) return false;
    uint32_t idx = get32le(p + 4);
    Session* s = nullptr;
    if (cur_.valid && cur_.localIdx == idx) s = &cur_;
    else if (next_.valid && next_.localIdx == idx) s = &next_;
    else if (prev_.valid && prev_.localIdx == idx) s = &prev_;
    if (!s || nowMs() - s->born > 180000) return false;
    uint64_t ctr = get64le(p + 8);
    uint8_t nonce[12];
    nonceLE(nonce, ctr);
    Bytes plain(n - 32);
    if (!aeadOpen(s->recvKey, nonce, p + 16, n - 16, nullptr, 0, plain.data())) return false;
    if (!replayOk(*s, ctr)) return false;
    lastRecv_ = nowMs();
    if (s == &next_) {   // the initiator used the session we answered: it becomes current
        prev_ = cur_;
        cur_ = next_;
        cur_.confirmed = true;
        next_ = Session();
        s = &cur_;
        flushQueue();
    }
    if (plain.size() < 20) return true;   // keepalive
    size_t len = plain.size();
    if ((plain[0] >> 4) == 4) {
        size_t ipLen = (size_t)plain[2] << 8 | plain[3];
        if (ipLen <= len) len = ipLen;
    }
    plain.resize(len);
    deliver_.push_back(plain);   // handed to onIp_ after the lock is released (TCP locks in the other order)
    return true;
}

void WgPeer::encryptAndSend(Session& s, const uint8_t* p, size_t n) {
    size_t padded = (n + 15) / 16 * 16;
    Bytes msg(16 + padded + 16, 0);
    msg[0] = 4;
    put32le(&msg[4], s.remoteIdx);
    uint64_t ctr = s.sendCtr++;
    for (int i = 0; i < 8; i++) msg[8 + i] = (uint8_t)(ctr >> (8 * i));
    Bytes plain(padded, 0);
    if (n) memcpy(plain.data(), p, n);
    uint8_t nonce[12];
    nonceLE(nonce, ctr);
    aeadSeal(s.sendKey, nonce, plain.data(), padded, nullptr, 0, &msg[16]);
    lastSend_ = nowMs();
    send_(msg.data(), msg.size());
}

void WgPeer::flushQueue() {
    while (!queue_.empty() && cur_.valid && cur_.confirmed) {
        Bytes b = queue_.front();
        queue_.pop_front();
        encryptAndSend(cur_, b.data(), b.size());
    }
}

bool WgPeer::receive(const uint8_t* p, size_t n) {
    std::vector<Bytes> out;
    bool handled = false;
    {
        std::lock_guard<std::recursive_mutex> l(mu_);
        if (n < 4) return false;
        switch (p[0]) {
        case 1: handled = handleInitiation(p, n); break;
        case 2: handled = handleResponse(p, n); break;
        case 4: handled = handleData(p, n); break;
        default: break;   // 3 = cookie reply: we never send under load
        }
        out.swap(deliver_);
    }
    for (size_t i = 0; i < out.size(); i++) onIp_(out[i].data(), out[i].size());
    return handled;
}

void WgPeer::sendIp(const uint8_t* p, size_t n) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    int64_t now = nowMs();
    bool usable = cur_.valid && cur_.confirmed && now - cur_.born < 180000 && cur_.sendCtr < (1ULL << 60);
    if (usable) {
        encryptAndSend(cur_, p, n);
        if (now - cur_.born > 120000) startHandshake();
        return;
    }
    if (queue_.size() < 256) queue_.push_back(Bytes(p, p + n));
    startHandshake();
}

void WgPeer::tick() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    int64_t now = nowMs();
    if (!queue_.empty() || hsPending_) {
        if (hsPending_ && now - hsSent_ > 5000) {
            hsPending_ = false;
            if (!queue_.empty()) startHandshake();
        } else if (!hsPending_) {
            startHandshake();
        }
    }
    if (cur_.valid && cur_.confirmed) {
        int64_t age = now - cur_.born;
        // passive keepalive: data came in and we had nothing to answer for 10 s
        if (lastRecv_ > lastSend_ && now - lastRecv_ > 10000 && age < 180000) encryptAndSend(cur_, nullptr, 0);
        // a session in use nears its end: renew it before the peer has to
        if (age > 165000 && age < 180000 && now - lastRecv_ < 30000 && !hsPending_) startHandshake();
    }
}

}  // namespace ts
