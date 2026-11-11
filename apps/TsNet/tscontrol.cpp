#include "tscontrol.h"

#include <cstring>

namespace ts {

static const char* kHost = "controlplane.tailscale.com";
static const unsigned kCapVer = 148;   // tailcfg.CurrentCapabilityVersion of Tailscale 1.104
// controlplane.tailscale.com/key?v=148 "publicKey" (the machine key of Tailscale's control server)
static const char* kControlKey = "7d2792f9c98d753d2042471536801949104c247f95eac770f8fb321595e2173b";

enum { F_DATA = 0, F_HEADERS = 1, F_RST = 3, F_SETTINGS = 4, F_PING = 6, F_GOAWAY = 7, F_WINDOW = 8, F_CONT = 9 };

static void put16be(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32be(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32be(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void nonceBE(uint8_t n[12], uint64_t c) {
    memset(n, 0, 4);
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(c >> (56 - 8 * i));
}

namespace {
struct Sym {   // Noise symmetric state
    uint8_t h[32], ck[32];
    void init() {
        const char* name = "Noise_IK_25519_ChaChaPoly_BLAKE2s";
        blake2s(h, name, strlen(name));
        memcpy(ck, h, 32);
    }
    void mixHash(const uint8_t* p, size_t n) { blake2s(h, h, 32, p, n); }
    bool mixDH(const uint8_t priv[32], const uint8_t pub[32], uint8_t k[32]) {
        uint8_t dh[32];
        if (!x25519(dh, priv, pub)) return false;
        uint8_t nck[32];
        kdf(ck, dh, 32, nck, k);
        memcpy(ck, nck, 32);
        return true;
    }
    void encryptAndHash(const uint8_t k[32], const uint8_t* p, size_t n, uint8_t* out) {
        uint8_t nonce[12] = {0};
        aeadSeal(k, nonce, p, n, h, 32, out);
        mixHash(out, n + 16);
    }
    bool decryptAndHash(const uint8_t k[32], const uint8_t* c, size_t n, uint8_t* out) {
        uint8_t nonce[12] = {0};
        if (!aeadOpen(k, nonce, c, n, h, 32, out)) return false;
        mixHash(c, n);
        return true;
    }
};
}  // namespace

Control::~Control() { close(); }

void Control::close() {
    std::lock_guard<std::mutex> l(closeMu_);   // the run loop and kick() may both close
    alive_ = false;
    if (tcp_) tcp_->close();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    cv_.notify_all();
}

bool Control::connect(const uint8_t machinePriv[32], std::string& err) {
    uint8_t controlPub[32], machinePub[32], ePriv[32], ePub[32], k[32];
    unhex(kControlKey, controlPub, 32);
    x25519Base(machinePub, machinePriv);
    newKeyPair(ePriv, ePub);

    Sym s;
    s.init();
    char prologue[48];
    snprintf(prologue, sizeof prologue, "Tailscale Control Protocol v%u", kCapVer);
    s.mixHash((const uint8_t*)prologue, strlen(prologue));
    s.mixHash(controlPub, 32);
    uint8_t init[101];
    put16be(init, kCapVer);
    init[2] = 1;
    put16be(init + 3, 96);
    memcpy(init + 5, ePub, 32);
    s.mixHash(ePub, 32);
    if (!s.mixDH(ePriv, controlPub, k)) { err = "noise es"; return false; }
    s.encryptAndHash(k, machinePub, 32, init + 37);
    if (!s.mixDH(machinePriv, controlPub, k)) { err = "noise ss"; return false; }
    s.encryptAndHash(k, nullptr, 0, init + 85);

    tcp_ = TcpStream::connect(kHost, 80, 10000, err);
    if (!tcp_) return false;
    std::string req = std::string("POST /ts2021 HTTP/1.1\r\nHost: ") + kHost +
                      "\r\nUser-Agent: Go-http-client/1.1\r\nContent-Length: 0\r\nConnection: upgrade\r\n"
                      "Upgrade: tailscale-control-protocol\r\nX-Tailscale-Handshake: " + base64(init, sizeof init) + "\r\n\r\n";
    std::string head;
    if (!tcp_->write(req) || !readHttpHead(*tcp_, head, 10000)) { err = "controllo: nessuna risposta"; return false; }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
        err = "controllo: " + head.substr(0, head.find('\r'));
        return false;
    }
    uint8_t resp[51];
    if (!tcp_->readFull(resp, 3, 10000)) { err = "controllo: handshake interrotto"; return false; }
    unsigned len = resp[1] << 8 | resp[2];
    if (resp[0] != 2 || len != 48) {
        std::string msg(len, '\0');
        if (len && len < 4096) tcp_->readFull((uint8_t*)&msg[0], len, 5000);
        err = "controllo rifiuta: " + msg;
        return false;
    }
    if (!tcp_->readFull(resp + 3, 48, 10000)) { err = "controllo: handshake interrotto"; return false; }
    s.mixHash(resp + 3, 32);
    if (!s.mixDH(ePriv, resp + 3, k) || !s.mixDH(machinePriv, resp + 3, k)) { err = "noise ee/se"; return false; }
    uint8_t dummy[1];
    if (!s.decryptAndHash(k, resp + 35, 16, dummy)) { err = "noise: chiave del server sbagliata"; return false; }
    kdf(s.ck, nullptr, 0, txKey_, rxKey_);
    txNonce_ = rxNonce_ = 0;
    rxPlain_.clear();
    rxOff_ = 0;

    // optional early payload ("\xff\xff\xffTS" + BE32 length + JSON), otherwise those 9 bytes are HTTP/2
    uint8_t hdr[9];
    if (!noiseRead(hdr, 9)) { err = "controllo: chiuso dopo l'handshake"; return false; }
    if (memcmp(hdr, "\xff\xff\xffTS", 5) == 0) {
        uint32_t n = get32be(hdr + 5);
        std::string ep(n, '\0');
        if (n > (1u << 20) || (n && !noiseRead((uint8_t*)&ep[0], n))) { err = "controllo: early payload"; return false; }
    } else {
        rxPlain_.insert(rxOff_, std::string((const char*)hdr, 9));
    }

    alive_ = true;
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    uint8_t settings[12] = {0, 2, 0, 0, 0, 0, 0, 4, 0x7f, 0xff, 0xff, 0xff};   // no push, max stream window
    uint8_t win[4];
    put32be(win, 0x7fffffff - 65535);
    if (!noiseWrite((const uint8_t*)preface, sizeof preface - 1) || !frame(F_SETTINGS, 0, 0, settings, 12) ||
        !frame(F_WINDOW, 0, 0, win, 4)) {
        err = "controllo: scrittura fallita";
        alive_ = false;
        return false;
    }
    reader_ = std::thread(&Control::readerLoop, this);
    return true;
}

bool Control::noiseWrite(const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> l(wmu_);
    uint8_t buf[4096];
    while (n > 0) {
        size_t k = n > 4077 ? 4077 : n;
        buf[0] = 4;
        put16be(buf + 1, (unsigned)(k + 16));
        uint8_t nonce[12];
        nonceBE(nonce, txNonce_++);
        aeadSeal(txKey_, nonce, p, k, nullptr, 0, buf + 3);
        if (!tcp_->write(buf, k + 19)) return false;
        p += k;
        n -= k;
    }
    return true;
}

// reader thread only (plus connect, before the thread starts)
bool Control::noiseRead(uint8_t* p, size_t n) {
    while (n > 0) {
        if (rxOff_ < rxPlain_.size()) {
            size_t k = rxPlain_.size() - rxOff_;
            if (k > n) k = n;
            memcpy(p, rxPlain_.data() + rxOff_, k);
            rxOff_ += k;
            p += k;
            n -= k;
            continue;
        }
        uint8_t hdr[3], buf[4096], plain[4096];
        if (!tcp_->readFull(hdr, 3, 24 * 3600 * 1000)) return false;   // close() unblocks it
        unsigned len = hdr[1] << 8 | hdr[2];
        if (hdr[0] != 4 || len < 16 || len > 4093 || !tcp_->readFull(buf, len, 30000)) return false;
        uint8_t nonce[12];
        nonceBE(nonce, rxNonce_++);
        if (!aeadOpen(rxKey_, nonce, buf, len, nullptr, 0, plain)) return false;
        rxPlain_.assign((const char*)plain, len - 16);
        rxOff_ = 0;
    }
    return true;
}

bool Control::frame(uint8_t type, uint8_t flags, unsigned stream, const uint8_t* p, size_t n) {
    std::string f(9 + n, '\0');
    uint8_t* b = (uint8_t*)&f[0];
    b[0] = (uint8_t)(n >> 16); b[1] = (uint8_t)(n >> 8); b[2] = (uint8_t)n;
    b[3] = type;
    b[4] = flags;
    put32be(b + 5, stream & 0x7fffffff);
    if (n) memcpy(b + 9, p, n);
    return noiseWrite(b, f.size());
}

static void hpackStr(std::string& o, const std::string& s) {
    size_t n = s.size();
    if (n < 127) o += (char)n;
    else {
        o += (char)127;
        n -= 127;
        while (n >= 128) { o += (char)(n % 128 + 128); n /= 128; }
        o += (char)n;
    }
    o += s;
}

static void hpackField(std::string& o, const char* name, const std::string& value) {
    o += '\0';   // literal header field without indexing, new name
    hpackStr(o, name);
    hpackStr(o, value);
}

unsigned Control::post(const std::string& path, const std::string& json) {
    std::string h;
    hpackField(h, ":method", "POST");
    hpackField(h, ":scheme", "https");
    hpackField(h, ":authority", kHost);
    hpackField(h, ":path", path);
    hpackField(h, "content-type", "application/json");
    char cl[16];
    snprintf(cl, sizeof cl, "%u", (unsigned)json.size());
    hpackField(h, "content-length", cl);
    hpackField(h, "user-agent", "Go-http-client/2.0");
    std::lock_guard<std::mutex> pl(postMu_);   // new stream ids must reach the server in increasing order
    unsigned id;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!alive_) return 0;
        id = nextId_;
        nextId_ += 2;
        streams_[id];
    }
    bool ok = frame(F_HEADERS, 0x4, id, (const uint8_t*)h.data(), h.size());
    for (size_t off = 0; ok && off < json.size(); off += 16384) {
        size_t k = json.size() - off < 16384 ? json.size() - off : 16384;
        ok = frame(F_DATA, off + k == json.size() ? 0x1 : 0, id, (const uint8_t*)json.data() + off, k);
    }
    if (json.empty() && ok) ok = frame(F_DATA, 0x1, id, nullptr, 0);
    if (!ok) {
        log_("controllo: invio fallito");
        close();
        return 0;
    }
    return id;
}

int Control::status(unsigned id, int timeoutMs) {
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] {
        return !alive_ || streams_[id].status != 0 || streams_[id].ended;
    });
    H2Stream& st = streams_[id];
    if (st.status) return st.status;
    return alive_ && !st.ended ? 0 : -1;
}

int Control::readBody(unsigned id, uint8_t* p, size_t n, int timeoutMs) {
    std::unique_lock<std::mutex> l(mu_);
    bool got = cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] {
        H2Stream& st = streams_[id];
        return !alive_ || st.ended || st.off < st.buf.size();
    });
    H2Stream& st = streams_[id];
    if (st.off < st.buf.size()) {
        size_t k = st.buf.size() - st.off;
        if (k > n) k = n;
        memcpy(p, st.buf.data() + st.off, k);
        st.off += k;
        if (st.off > 65536 && st.off * 2 > st.buf.size()) {
            st.buf.erase(0, st.off);
            st.off = 0;
        }
        return (int)k;
    }
    return got ? 0 : -1;
}

bool Control::readBodyFull(unsigned id, uint8_t* p, size_t n, int timeoutMs) {
    int64_t end = nowMs() + timeoutMs;
    while (n > 0) {
        int left = (int)(end - nowMs());
        if (left <= 0) return false;
        int r = readBody(id, p, n, left);
        if (r == 0) return false;
        if (r < 0) continue;
        p += r;
        n -= r;
    }
    return true;
}

std::string Control::readAll(unsigned id, int timeoutMs) {
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return !alive_ || streams_[id].ended; });
    H2Stream& st = streams_[id];
    return st.buf.substr(st.off);
}

void Control::forget(unsigned id) {
    std::lock_guard<std::mutex> l(mu_);
    streams_.erase(id);
}

// :status from the first header of a response block: Go's encoder sends 200 as static index 8 and other codes as
// a literal with the :status name index and a plain (non-Huffman, as short as Huffman) value
std::string lastUnknownStatus;

static int parseStatus(const uint8_t* p, size_t n) {
    static const int idx[] = {200, 204, 206, 304, 400, 404, 500};
    while (n > 0 && (p[0] & 0xe0) == 0x20) {   // dynamic table size updates come first
        bool more = (p[0] & 0x1f) == 0x1f;
        p++; n--;
        while (more && n > 0) { more = (p[0] & 0x80) != 0; p++; n--; }
    }
    if (n == 0) return 0;
    if ((p[0] & 0x80) && (p[0] & 0x7f) >= 8 && (p[0] & 0x7f) <= 14) return idx[(p[0] & 0x7f) - 8];
    uint8_t nameIdx = (p[0] & 0xc0) == 0x40 ? (p[0] & 0x3f) : (p[0] & 0x0f);
    if (nameIdx >= 8 && nameIdx <= 14 && n >= 5 && !(p[1] & 0x80) && p[1] == 3)
        return (p[2] - '0') * 100 + (p[3] - '0') * 10 + (p[4] - '0');
    char b[64];
    snprintf(b, sizeof b, "%02x %02x %02x %02x", p[0], n > 1 ? p[1] : 0, n > 2 ? p[2] : 0, n > 3 ? p[3] : 0);
    lastUnknownStatus = b;
    return 999;
}

void Control::readerLoop() {
    std::string payload;
    while (alive_) {
        uint8_t h[9];
        if (!noiseRead(h, 9)) break;
        size_t len = (size_t)h[0] << 16 | h[1] << 8 | h[2];
        uint8_t type = h[3], flags = h[4];
        unsigned id = get32be(h + 5) & 0x7fffffff;
        if (len > (1u << 20)) break;
        payload.resize(len);
        if (len && !noiseRead((uint8_t*)&payload[0], len)) break;
        const uint8_t* p = (const uint8_t*)payload.data();
        size_t n = len;
        if ((type == F_DATA || type == F_HEADERS) && (flags & 0x8) && n > 0) {   // padded
            size_t pad = p[0];
            p++;
            n = n - 1 >= pad ? n - 1 - pad : 0;
        }
        if (type == F_HEADERS && (flags & 0x20) && n >= 5) { p += 5; n -= 5; }   // priority
        switch (type) {
        case F_DATA:
        case F_HEADERS:
        case F_CONT: {
            std::lock_guard<std::mutex> l(mu_);
            std::map<unsigned, H2Stream>::iterator it = streams_.find(id);
            if (it == streams_.end()) break;
            if (type == F_DATA) it->second.buf.append((const char*)p, n);
            else if (type == F_HEADERS && it->second.status == 0) it->second.status = parseStatus(p, n);
            if ((flags & 0x1) && type != F_CONT) it->second.ended = true;
            cv_.notify_all();
            break;
        }
        case F_RST: {
            std::lock_guard<std::mutex> l(mu_);
            std::map<unsigned, H2Stream>::iterator it = streams_.find(id);
            if (it != streams_.end()) {
                it->second.ended = true;
                if (!it->second.status) it->second.status = -1;
            }
            cv_.notify_all();
            break;
        }
        case F_SETTINGS:
            if (!(flags & 0x1)) frame(F_SETTINGS, 0x1, 0, nullptr, 0);
            break;
        case F_PING:
            if (!(flags & 0x1) && n == 8) frame(F_PING, 0x1, 0, p, 8);
            break;
        case F_GOAWAY:
            log_("controllo: GOAWAY");
            break;
        default:
            break;
        }
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        alive_ = false;
    }
    cv_.notify_all();
    log_("controllo: connessione chiusa");
}

}  // namespace ts
