#include "engine.h"

#include "crypto.h"
#include "mtproto_auth.h"
#include "mtproto_dh.h"
#include "mtproto_msg.h"
#include "mtproto_rpc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace tg {

namespace {

const uint32_t kRpcErrorId = 0x2144ca19u;
const uint32_t kGzipPackedId = 0x3072cfa1u;
const uint32_t kReqDhParamsId = 0xd712e4beu;
const uint32_t kDhGenRetryId = 0x46dc1fb9u;
const uint32_t kDhGenFailId = 0xa69dae02u;
const int64_t kConnectTimeoutMs = 15000;
const int64_t kRequestTimeoutMs = 45000;
const int64_t kPingEveryMs = 30000;
const int64_t kSilenceMs = 90000;

const char* kProdKeyHex =
    "e8bb3305c0b52c6cf2afdf7637313489e63e05268e5badb601af417786472e5f93b85438968e20e6729a301c0afc121bf7151f8344"
    "36f7fda680847a66bf64accec78ee21c0b316f0edafe2f41908da7bd1f4a5107638eeb67040ace472a14f90d9f7c2b7def99688ba3"
    "073adb5750bb02964902a359fe745d8170e36876d4fd8a5d41b2a76cbff9a13267eb9580b2d06d10357448d20d9da2191cb5d8c939"
    "82961cdfdeda629e37f1fb09a0722027696032fe61ed663db7a37f6f263d370f69db53a0dc0a1748bdaaff6209d5645485e6e001d1"
    "953255757e4b8e42813347b11da6ab500fd0ace7e6dfa3736199ccaf9397ed0745a427dcfa6cd67bcb1acff3";
const char* kTestKeyHex =
    "c8c11d635691fac091dd9489aedced2932aa8a0bcefef05fa800892d9b52ed03200865c9e97211cb2ee6c7ae96d3fb0e15aeffd660"
    "19b44a08a240cfdd2868a85e1f54d6fa5deaa041f6941ddf302690d61dc476385c2fa655142353cb4e4b59f6e5b6584db76fe8b137"
    "0263246c010c93d011014113ebdf987d093f9d37c2be48352d69a1683f8f6e6c2167983c761e3ab169fde5daaa12123fa1beab621"
    "e4da5935e9c198f82f35eae583a99386d8110ea6bd1abb0f568759f62694419ea5f69847c43462abef858b4cb5edc84e7b9226cd7b"
    "d7e183aa974a712c079dde85b9dc063b8a5c08e8f859c0ee5dcd824c7807f20153361a7f63cfd2a433a1be7f5";

std::vector<uint8_t> fromHex(const std::string& h)
{
    std::vector<uint8_t> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::strtoul(h.substr(i, 2).c_str(), 0, 16)));
    return out;
}

std::string toHex(const uint8_t* p, std::size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

void putLe32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void putLe64(std::vector<uint8_t>& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

std::vector<uint8_t> minimalBe(uint64_t v)
{
    std::vector<uint8_t> o;
    for (int i = 7; i >= 0; --i) {
        uint8_t b = static_cast<uint8_t>(v >> (8 * i));
        if (b || !o.empty()) o.push_back(b);
    }
    return o;
}

uint64_t beToU64(const std::vector<uint8_t>& b)
{
    uint64_t v = 0;
    for (std::size_t i = 0; i < b.size(); ++i) v = (v << 8) | b[i];
    return v;
}

// "FILE_MIGRATE_4" -> ("FILE_MIGRATE_", 4); "FLOOD_WAIT_30" -> ("FLOOD_WAIT_", 30)
bool splitNumber(const std::string& m, std::string& prefix, int& n)
{
    std::size_t p = m.find_last_of('_');
    if (p == std::string::npos || p + 1 >= m.size()) return false;
    for (std::size_t i = p + 1; i < m.size(); ++i) if (m[i] < '0' || m[i] > '9') return false;
    prefix = m.substr(0, p + 1);
    n = std::atoi(m.c_str() + p + 1);
    return true;
}

} // namespace

Engine::DcKey::DcKey() : has(false), keyId(0), salt(0), imported(false) { std::memset(key, 0, sizeof(key)); }

Engine::Conn::Conn()
    : dc(0), st(Idle), hsStep(0), sessionId(0), lastMsgId(0), seq(0), initSent(false), importing(false),
      stateSinceMs(0), lastRxMs(0), lastPingMs(0), retryAtMs(0), failures(0)
{
    std::memset(nonce, 0, sizeof(nonce));
    std::memset(serverNonce, 0, sizeof(serverNonce));
    std::memset(newNonce, 0, sizeof(newNonce));
}

Engine::Engine(IEngineHost* host, const EngineConfig& cfg)
    : host_(host), cfg_(cfg), homeDc_(2), authorized_(false), running_(false), timeOffsetMs_(0), nextReqId_(1)
{
    if (cfg.testMode) {
        addrs_[1] = std::make_pair(std::string("149.154.175.10"), 443);
        addrs_[2] = std::make_pair(std::string("149.154.167.40"), 443);
        addrs_[3] = std::make_pair(std::string("149.154.175.117"), 443);
    } else {
        addrs_[1] = std::make_pair(std::string("149.154.175.53"), 443);
        addrs_[2] = std::make_pair(std::string("149.154.167.51"), 443);
        addrs_[3] = std::make_pair(std::string("149.154.175.100"), 443);
        addrs_[4] = std::make_pair(std::string("149.154.167.91"), 443);
        addrs_[5] = std::make_pair(std::string("91.108.56.130"), 443);
    }
}

// ---------------------------------------------------------------- state

std::string Engine::save() const
{
    std::ostringstream o;
    o << "v 1\nhome " << homeDc_ << "\nauthorized " << (authorized_ ? 1 : 0) << "\noffset " << timeOffsetMs_ << "\n";
    for (std::map<int, DcKey>::const_iterator it = keys_.begin(); it != keys_.end(); ++it) {
        if (!it->second.has) continue;
        o << "key " << it->first << " " << toHex(it->second.key, 256) << " " << it->second.salt << " "
          << (it->second.imported ? 1 : 0) << "\n";
    }
    for (std::map<int, std::pair<std::string, int> >::const_iterator it = addrs_.begin(); it != addrs_.end(); ++it)
        o << "addr " << it->first << " " << it->second.first << " " << it->second.second << "\n";
    return o.str();
}

bool Engine::load(const std::string& blob)
{
    std::istringstream in(blob);
    std::string tag;
    bool any = false;
    while (in >> tag) {
        if (tag == "home") in >> homeDc_;
        else if (tag == "authorized") { int a = 0; in >> a; authorized_ = a != 0; }
        else if (tag == "offset") { long long v = 0; in >> v; timeOffsetMs_ = v; }
        else if (tag == "key") {
            int dc = 0, imp = 0; std::string hex; unsigned long long salt = 0;
            in >> dc >> hex >> salt >> imp;
            std::vector<uint8_t> k = fromHex(hex);
            if (k.size() != 256) continue;
            DcKey& d = keys_[dc];
            d.has = true;
            std::memcpy(d.key, &k[0], 256);
            d.keyId = deriveAuthKeyId(d.key);
            d.salt = salt;
            d.imported = imp != 0;
            any = true;
        } else if (tag == "addr") {
            int dc = 0, port = 0; std::string ip;
            in >> dc >> ip >> port;
            if (!ip.empty()) addrs_[dc] = std::make_pair(ip, port);
        } else {
            std::string rest; std::getline(in, rest);
        }
    }
    return any;
}

void Engine::setAuthorized(bool v)
{
    authorized_ = v;
    if (v) key(homeDc_).imported = true;
    host_->onStateChanged();
}

void Engine::setDcAddress(int dc, const std::string& ip, int port)
{
    addrs_[dc] = std::make_pair(ip, port);
}

void Engine::logOutLocal()
{
    for (std::map<int, Conn>::iterator it = conns_.begin(); it != conns_.end(); ++it) closeConn(it->second, "logout");
    keys_.clear();
    authorized_ = false;
    host_->onStateChanged();
}

bool Engine::online() const
{
    std::map<int, Conn>::const_iterator it = conns_.find(homeDc_);
    return it != conns_.end() && it->second.st == Conn::Ready;
}

// ---------------------------------------------------------------- requests

uint64_t Engine::invoke(const TlValue& fn, const RpcCallback& cb, int dc)
{
    Req r;
    r.id = nextReqId_++;
    r.fn = fn;
    r.cb = cb;
    r.dc = dc == 0 ? homeDc_ : dc;
    reqs_[r.id] = r;
    Conn& c = conn(r.dc);
    if (c.st == Conn::Ready) pump(c);
    else if (c.st == Conn::Idle && c.retryAtMs <= now()) connectDc(r.dc);
    return r.id;
}

void Engine::cancel(uint64_t reqId)
{
    reqs_.erase(reqId);
}

void Engine::finish(uint64_t reqId, const TlValue& v, const RpcErr& e)
{
    std::map<uint64_t, Req>::iterator it = reqs_.find(reqId);
    if (it == reqs_.end()) return;
    RpcCallback cb = it->second.cb;
    reqs_.erase(it);
    if (cb) cb(v, e);
}

Engine::Req* Engine::findByMsg(uint64_t msgId)
{
    for (std::map<uint64_t, Req>::iterator it = reqs_.begin(); it != reqs_.end(); ++it)
        if (it->second.msgId == msgId) return &it->second;
    return 0;
}

bool Engine::needsImport(int dc) const
{
    if (!authorized_ || dc == homeDc_) return false;
    std::map<int, DcKey>::const_iterator it = keys_.find(dc);
    return it == keys_.end() || !it->second.imported;
}

void Engine::startImport(int dc)
{
    Conn& c = conn(dc);
    if (c.importing) return;
    c.importing = true;
    log("export authorization for DC " + std::to_string(static_cast<long long>(dc)));
    invoke(tlObj("auth.exportAuthorization").set("dc_id", tlInt(dc)), [this, dc](const TlValue& v, const RpcErr& e) {
        if (!e.ok()) {
            conn(dc).importing = false;
            log("exportAuthorization failed: " + e.message);
            for (std::map<uint64_t, Req>::iterator it = reqs_.begin(); it != reqs_.end();) {
                std::map<uint64_t, Req>::iterator cur = it++;
                if (cur->second.dc == dc) finish(cur->first, TlValue(), e);
            }
            return;
        }
        Req r;
        r.id = nextReqId_++;
        r.fn = tlObj("auth.importAuthorization").set("id", v["id"]).set("bytes", v["bytes"]);
        r.dc = dc;
        r.isImport = true;
        r.cb = [this, dc](const TlValue&, const RpcErr& e2) {
            conn(dc).importing = false;
            if (e2.ok()) {
                key(dc).imported = true;
                host_->onStateChanged();
                pump(conn(dc));
            } else {
                log("importAuthorization failed: " + e2.message);
            }
        };
        reqs_[r.id] = r;
        Conn& cc = conn(dc);
        if (cc.st == Conn::Ready) pump(cc);
        else if (cc.st == Conn::Idle) connectDc(dc);
    }, homeDc_);
}

void Engine::pump(Conn& c)
{
    if (c.st != Conn::Ready) return;
    int64_t t = now();
    bool blocked = needsImport(c.dc);
    if (blocked) {
        bool want = false;
        for (std::map<uint64_t, Req>::iterator it = reqs_.begin(); it != reqs_.end(); ++it)
            if (it->second.dc == c.dc && !it->second.isImport) want = true;
        if (want) startImport(c.dc);
    }
    // collect first: sending may not change reqs_, but keep the loop simple and ordered
    std::vector<uint64_t> todo;
    for (std::map<uint64_t, Req>::iterator it = reqs_.begin(); it != reqs_.end(); ++it) {
        Req& r = it->second;
        if (r.dc != c.dc || r.msgId != 0 || r.notBeforeMs > t) continue;
        if (blocked && !r.isImport) continue;
        todo.push_back(it->first);
    }
    for (std::size_t i = 0; i < todo.size(); ++i) {
        std::map<uint64_t, Req>::iterator it = reqs_.find(todo[i]);
        if (it == reqs_.end()) continue;
        Req& r = it->second;
        std::vector<uint8_t> body;
        try {
            if (!c.initSent) {
                TlValue init = tlObj("initConnection");
                init.set("api_id", tlInt(cfg_.apiId))
                    .set("device_model", tlStr(cfg_.deviceModel))
                    .set("system_version", tlStr(cfg_.systemVersion))
                    .set("app_version", tlStr(cfg_.appVersion))
                    .set("system_lang_code", tlStr(cfg_.langCode))
                    .set("lang_pack", tlStr(""))
                    .set("lang_code", tlStr(cfg_.langCode))
                    .set("query", r.fn);
                body = tlSerialize(tlObj("invokeWithLayer").set("layer", tlInt(tlLayer())).set("query", init));
                c.initSent = true;
            } else {
                body = tlSerialize(r.fn);
            }
        } catch (const std::exception& e) {
            finish(r.id, TlValue(), RpcErr(-1, std::string("serialize: ") + e.what()));
            continue;
        }
        r.msgId = sendEncrypted(c, body, true);
        r.sentMs = t;
        ++r.tries;
    }
    flushAcks(c);
}

void Engine::flushAcks(Conn& c)
{
    if (c.acks.empty() || c.st != Conn::Ready) return;
    std::vector<uint64_t> ids;
    ids.swap(c.acks);
    sendEncrypted(c, buildMsgsAck(ids), false);
}

void Engine::resendAll(int dc)
{
    for (std::map<uint64_t, Req>::iterator it = reqs_.begin(); it != reqs_.end(); ++it)
        if (it->second.dc == dc) it->second.msgId = 0;
}

// ---------------------------------------------------------------- connections

Engine::Conn& Engine::conn(int dc)
{
    std::map<int, Conn>::iterator it = conns_.find(dc);
    if (it == conns_.end()) {
        Conn c;
        c.dc = dc;
        it = conns_.insert(std::make_pair(dc, c)).first;
    }
    return it->second;
}

void Engine::connectDc(int dc)
{
    Conn& c = conn(dc);
    if (c.st != Conn::Idle) return;
    std::map<int, std::pair<std::string, int> >::iterator a = addrs_.find(dc);
    if (a == addrs_.end()) {
        log("no address for DC " + std::to_string(static_cast<long long>(dc)));
        for (std::map<uint64_t, Req>::iterator it = reqs_.begin(); it != reqs_.end();) {
            std::map<uint64_t, Req>::iterator cur = it++;
            if (cur->second.dc == dc) finish(cur->first, TlValue(), RpcErr(-1, "NO_DC_ADDRESS"));
        }
        return;
    }
    c.st = Conn::Connecting;
    c.stateSinceMs = now();
    c.rx.clear();
    log("connect DC " + std::to_string(static_cast<long long>(dc)) + " " + a->second.first);
    host_->netConnect(dc, a->second.first, a->second.second);
}

void Engine::closeConn(Conn& c, const char* why)
{
    if (c.st == Conn::Idle) return;
    log(std::string("close DC ") + std::to_string(static_cast<long long>(c.dc)) + ": " + why);
    bool wasReady = c.st == Conn::Ready;
    c.st = Conn::Idle;
    c.rx.clear();
    c.acks.clear();
    host_->netClose(c.dc);
    resendAll(c.dc);
    ++c.failures;
    int64_t backoff = std::min<int64_t>(30000, 500LL << std::min(c.failures, 6));
    c.retryAtMs = now() + backoff;
    if (wasReady && c.dc == homeDc_) host_->onOnline(false);
}

void Engine::start()
{
    running_ = true;
    conn(homeDc_).retryAtMs = 0;
    conn(homeDc_).failures = 0;
    connectDc(homeDc_);
}

void Engine::stop()
{
    running_ = false;
    for (std::map<int, Conn>::iterator it = conns_.begin(); it != conns_.end(); ++it) {
        closeConn(it->second, "stop");
        it->second.failures = 0;
        it->second.retryAtMs = 0;
    }
}

void Engine::onConnected(int dc)
{
    Conn& c = conn(dc);
    if (c.st != Conn::Connecting) return;
    static const uint8_t marker[4] = {0xee, 0xee, 0xee, 0xee};
    host_->netSend(dc, std::vector<uint8_t>(marker, marker + 4));
    c.lastRxMs = now();
    c.lastPingMs = now();
    c.initSent = false;
    c.seq = 0;
    c.lastMsgId = 0;
    host_->randomBytes(reinterpret_cast<uint8_t*>(&c.sessionId), 8);
    if (key(dc).has) {
        c.st = Conn::Ready;
        c.failures = 0;
        if (dc == homeDc_) host_->onOnline(true);
        pump(c);
    } else {
        handshakeStart(c);
    }
}

void Engine::onClosed(int dc)
{
    Conn& c = conn(dc);
    closeConn(c, "closed by peer");
}

void Engine::onData(int dc, const uint8_t* data, std::size_t size)
{
    Conn& c = conn(dc);
    if (c.st == Conn::Idle || c.st == Conn::Connecting) return;
    c.rx.insert(c.rx.end(), data, data + size);
    c.lastRxMs = now();
    while (c.rx.size() >= 4) {
        uint32_t len = le32(&c.rx[0]);
        if (len > 16 * 1024 * 1024) { closeConn(c, "bad frame"); return; }
        if (c.rx.size() < 4 + len) break;
        std::vector<uint8_t> packet(c.rx.begin() + 4, c.rx.begin() + 4 + len);
        c.rx.erase(c.rx.begin(), c.rx.begin() + 4 + len);
        if (packet.size() == 4) {
            int32_t code = static_cast<int32_t>(le32(&packet[0]));
            log("transport error " + std::to_string(static_cast<long long>(code)) + " on DC " + std::to_string(static_cast<long long>(dc)));
            if (code == -404 && c.st == Conn::Ready) {
                bool wasHomeAuth = dc == homeDc_ && authorized_;
                key(dc) = DcKey();
                host_->onStateChanged();
                if (wasHomeAuth) { authorized_ = false; host_->onAuthLost(); }
            }
            closeConn(c, "transport error");
            return;
        }
        try {
            if (c.st == Conn::Handshake) handshakeStep(c, packet);
            else handlePacket(c, packet);
        } catch (const std::exception& e) {
            closeConn(c, e.what());
            return;
        }
        if (c.st == Conn::Idle) return;
    }
}

void Engine::tick()
{
    int64_t t = now();
    for (std::map<int, Conn>::iterator it = conns_.begin(); it != conns_.end(); ++it) {
        Conn& c = it->second;
        bool wanted = (running_ && c.dc == homeDc_);
        if (!wanted)
            for (std::map<uint64_t, Req>::iterator r = reqs_.begin(); r != reqs_.end(); ++r)
                if (r->second.dc == c.dc) { wanted = true; break; }
        if (c.st == Conn::Idle) {
            if (wanted && t >= c.retryAtMs) connectDc(c.dc);
            continue;
        }
        if (c.st == Conn::Connecting || c.st == Conn::Handshake) {
            if (t - c.stateSinceMs > kConnectTimeoutMs) closeConn(c, "connect timeout");
            continue;
        }
        if (t - c.lastRxMs > kSilenceMs) { closeConn(c, "silent"); continue; }
        bool stuck = false;
        for (std::map<uint64_t, Req>::iterator r = reqs_.begin(); r != reqs_.end(); ++r)
            if (r->second.dc == c.dc && r->second.msgId && t - r->second.sentMs > kRequestTimeoutMs) stuck = true;
        if (stuck) { closeConn(c, "request timeout"); continue; }
        if (t - c.lastPingMs > kPingEveryMs && (c.dc == homeDc_ || wanted)) {
            c.lastPingMs = t;
            uint64_t pingId = 0;
            host_->randomBytes(reinterpret_cast<uint8_t*>(&pingId), 8);
            sendEncrypted(c, tlSerialize(tlObj("ping_delay_disconnect").set("ping_id", tlLong(static_cast<int64_t>(pingId)))
                                             .set("disconnect_delay", tlInt(75))), true);
        }
        pump(c);
    }
}

// ---------------------------------------------------------------- wire

uint64_t Engine::nextMsgId(Conn& c)
{
    int64_t ms = now() + timeOffsetMs_;
    uint64_t sec = static_cast<uint64_t>(ms / 1000);
    uint64_t frac = (static_cast<uint64_t>(ms % 1000) << 32) / 1000;
    uint64_t id = ((sec << 32) | frac) & ~static_cast<uint64_t>(3);
    if (id <= c.lastMsgId) id = c.lastMsgId + 4;
    c.lastMsgId = id;
    return id;
}

void Engine::sendPlain(Conn& c, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> p;
    putLe64(p, 0);
    putLe64(p, nextMsgId(c));
    putLe32(p, static_cast<uint32_t>(body.size()));
    p.insert(p.end(), body.begin(), body.end());
    std::vector<uint8_t> frame;
    putLe32(frame, static_cast<uint32_t>(p.size()));
    frame.insert(frame.end(), p.begin(), p.end());
    host_->netSend(c.dc, frame);
}

uint64_t Engine::sendEncrypted(Conn& c, const std::vector<uint8_t>& body, bool contentRelated)
{
    DcKey& k = key(c.dc);
    Session s;
    std::memcpy(s.auth_key, k.key, 256);
    s.authKeyId = k.keyId;
    s.salt = k.salt;
    s.sessionId = c.sessionId;
    uint64_t msgId = nextMsgId(c);
    uint32_t seqNo = c.seq * 2 + (contentRelated ? 1 : 0);
    if (contentRelated) ++c.seq;
    std::size_t pad = 12;
    while ((32 + body.size() + pad) % 16 != 0) ++pad;
    uint8_t extra = 0;
    host_->randomBytes(&extra, 1);
    pad += (extra % 4) * 16;   // a little random length
    std::vector<uint8_t> padding(pad);
    host_->randomBytes(&padding[0], pad);
    std::vector<uint8_t> packet = encryptMessage(s, msgId, seqNo, body, &padding[0], pad);
    std::vector<uint8_t> frame;
    putLe32(frame, static_cast<uint32_t>(packet.size()));
    frame.insert(frame.end(), packet.begin(), packet.end());
    host_->netSend(c.dc, frame);
    return msgId;
}

// ---------------------------------------------------------------- handshake

void Engine::handshakeStart(Conn& c)
{
    c.st = Conn::Handshake;
    c.stateSinceMs = now();
    c.hsStep = 1;
    host_->randomBytes(c.nonce, 16);
    log("auth key: req_pq_multi on DC " + std::to_string(static_cast<long long>(c.dc)));
    sendPlain(c, buildReqPqMulti(c.nonce));
}

void Engine::handshakeStep(Conn& c, const std::vector<uint8_t>& packet)
{
    if (packet.size() < 20 || le64(&packet[0]) != 0) throw std::runtime_error("handshake: bad packet");
    uint32_t bodyLen = le32(&packet[16]);
    if (bodyLen != packet.size() - 20) throw std::runtime_error("handshake: bad length");
    std::vector<uint8_t> body(packet.begin() + 20, packet.end());

    if (c.hsStep == 1) {
        ResPQ res = parseResPQ(&body[0], body.size());
        if (std::memcmp(res.nonce, c.nonce, 16) != 0) throw std::runtime_error("handshake: nonce mismatch");
        std::memcpy(c.serverNonce, res.serverNonce, 16);
        std::vector<uint8_t> n = fromHex(cfg_.testMode ? kTestKeyHex : kProdKeyHex);
        uint64_t fp = rsaFingerprint(&n[0], n.size(), 65537);
        if (std::find(res.fingerprints.begin(), res.fingerprints.end(), fp) == res.fingerprints.end())
            throw std::runtime_error("handshake: RSA key not offered");
        uint64_t pq = beToU64(res.pq), p = 0, q = 0;
        factorPq(pq, p, q);
        if (p > q) std::swap(p, q);
        if (!p || p * q != pq) throw std::runtime_error("handshake: pq");
        std::vector<uint8_t> pb = minimalBe(p), qb = minimalBe(q);
        host_->randomBytes(c.newNonce, 32);
        int dcField = cfg_.testMode ? 10000 + c.dc : c.dc;
        std::vector<uint8_t> inner = buildPqInnerDataDc(res.pq, pb, qb, c.nonce, c.serverNonce, c.newNonce, dcField);
        uint8_t rnd[256];
        host_->randomBytes(rnd, sizeof(rnd));
        std::vector<uint8_t> enc = rsaPadAndEncrypt(&inner[0], inner.size(), &n[0], n.size(), 65537, rnd, sizeof(rnd));
        TlWriter w;
        w.writeConstructor(kReqDhParamsId);
        w.writeRaw(c.nonce, 16);
        w.writeRaw(c.serverNonce, 16);
        w.writeBytes(pb.empty() ? 0 : &pb[0], pb.size());
        w.writeBytes(qb.empty() ? 0 : &qb[0], qb.size());
        int64_t sfp;
        std::memcpy(&sfp, &fp, 8);
        w.writeInt64(sfp);
        w.writeBytes(&enc[0], enc.size());
        c.hsStep = 2;
        sendPlain(c, w.data());
        return;
    }
    if (c.hsStep == 2) {
        TlReader r(&body[0], body.size());
        if (r.readConstructor() != SERVER_DH_PARAMS_OK_ID) throw std::runtime_error("handshake: server_DH_params_fail");
        uint8_t n1[16], n2[16];
        r.readRaw(n1, 16);
        r.readRaw(n2, 16);
        if (std::memcmp(n1, c.nonce, 16) || std::memcmp(n2, c.serverNonce, 16)) throw std::runtime_error("handshake: nonce mismatch");
        std::string ans = r.readBytes();
        ServerDhInner dh = parseServerDhParams(reinterpret_cast<const uint8_t*>(ans.data()), ans.size(), c.nonce,
                                               c.serverNonce, c.newNonce);
        if (!checkDhParams(dh)) throw std::runtime_error("handshake: bad DH params");
        timeOffsetMs_ = static_cast<int64_t>(dh.serverTime) * 1000 - host_->unixTimeMs();
        uint8_t b[256];
        host_->randomBytes(b, sizeof(b));
        DcKey& k = key(c.dc);
        std::vector<uint8_t> gB = computeClientDh(dh, b, k.key, k.keyId);
        uint8_t pad[16];
        host_->randomBytes(pad, sizeof(pad));
        std::vector<uint8_t> enc = buildClientDhInnerEncrypted(dh, c.newNonce, gB, pad, sizeof(pad));
        TlWriter w;
        w.writeConstructor(SET_CLIENT_DH_PARAMS_ID);
        w.writeRaw(c.nonce, 16);
        w.writeRaw(c.serverNonce, 16);
        w.writeBytes(&enc[0], enc.size());
        c.hsStep = 3;
        sendPlain(c, w.data());
        return;
    }
    if (c.hsStep == 3) {
        TlReader r(&body[0], body.size());
        uint32_t id = r.readConstructor();
        if (id == kDhGenRetryId || id == kDhGenFailId) throw std::runtime_error("handshake: dh_gen retry/fail");
        if (id != DH_GEN_OK_ID) throw std::runtime_error("handshake: unexpected answer");
        uint8_t n1[16], n2[16], h[16];
        r.readRaw(n1, 16);
        r.readRaw(n2, 16);
        r.readRaw(h, 16);
        DcKey& k = key(c.dc);
        checkDhGenOk(c.newNonce, k.key, h);
        std::vector<uint8_t> salt = deriveServerSalt(c.newNonce, c.serverNonce);
        k.salt = le64(&salt[0]);
        k.has = true;
        k.imported = authorized_ && c.dc == homeDc_;
        log("auth key ready on DC " + std::to_string(static_cast<long long>(c.dc)));
        host_->onStateChanged();
        c.st = Conn::Ready;
        c.failures = 0;
        c.stateSinceMs = now();
        if (c.dc == homeDc_) host_->onOnline(true);
        pump(c);
    }
}

// ---------------------------------------------------------------- incoming

void Engine::handlePacket(Conn& c, const std::vector<uint8_t>& packet)
{
    DcKey& k = key(c.dc);
    Session s;
    std::memcpy(s.auth_key, k.key, 256);
    s.authKeyId = k.keyId;
    s.salt = k.salt;
    s.sessionId = c.sessionId;
    DecryptedMsg m;
    if (!decryptMessage(s, packet, m)) throw std::runtime_error("decrypt failed");
    if (m.sessionId != c.sessionId) { log("message of another session ignored"); return; }
    handleBody(c, m.msgId, m.seqNo, m.body);
    if (c.st == Conn::Ready) {
        if (c.acks.size() >= 16) flushAcks(c);
        pump(c);
    }
}

void Engine::handleBody(Conn& c, uint64_t msgId, uint32_t seqNo, const std::vector<uint8_t>& body)
{
    if (body.size() < 4) return;
    uint32_t id = le32(&body[0]);
    if ((seqNo & 1) && id != 0x73f1f8dcu) c.acks.push_back(msgId);
    if (id == kGzipPackedId) {
        TlReader r(&body[0], body.size());
        r.readConstructor();
        std::string packed = r.readBytes(), plain;
        if (!gunzipBytes(packed, plain)) throw std::runtime_error("gzip");
        std::vector<uint8_t> inner(plain.begin(), plain.end());
        handleBody(c, msgId, 0, inner);
        return;
    }
    if (id == 0x73f1f8dcu) {   // msg_container: vector<%Message{msg_id:long seqno:int bytes:int body}>
        std::size_t pos = 4;
        if (body.size() < 8) return;
        uint32_t n = le32(&body[pos]);
        pos += 4;
        for (uint32_t i = 0; i < n; ++i) {
            if (pos + 16 > body.size()) throw std::runtime_error("bad container");
            uint64_t cid = le64(&body[pos]);
            uint32_t cseq = le32(&body[pos + 8]);
            uint32_t len = le32(&body[pos + 12]);
            pos += 16;
            if (pos + len > body.size()) throw std::runtime_error("bad container");
            std::vector<uint8_t> child(body.begin() + pos, body.begin() + pos + len);
            pos += len;
            handleBody(c, cid, cseq, child);
        }
        return;
    }
    const TlCtorDef* def = tlFind(id);
    if (def && !def->isFunction && std::strcmp(def->type, "Updates") == 0) {
        TlValue u;
        try { u = tlParse(&body[0], body.size(), "Updates"); }
        catch (const std::exception& e) { log(std::string("updates parse: ") + e.what()); return; }
        host_->onUpdates(u);
        return;
    }
    RpcMessage rm;
    if (!parseServerMessage(body, rm)) {
        char buf[48];
        std::sprintf(buf, "unparsed service message 0x%08x", id);
        log(buf);
        return;
    }
    switch (rm.kind) {
    case RpcMessage::Container:
        for (std::size_t i = 0; i < rm.children.size(); ++i)
            handleBody(c, rm.children[i].msgId, static_cast<uint32_t>(rm.children[i].seqNo), rm.children[i].body);
        break;
    case RpcMessage::RpcResult:
        handleResult(c, rm.reqMsgId, rm.rawResult);
        break;
    case RpcMessage::BadServerSalt: {
        key(c.dc).salt = rm.newSalt;
        host_->onStateChanged();
        Req* r = findByMsg(rm.badMsgId);
        if (r) r->msgId = 0;
        break;
    }
    case RpcMessage::BadMsgNotification: {
        log("bad_msg_notification " + std::to_string(static_cast<long long>(rm.errorCode)));
        Req* r = findByMsg(rm.badMsgId);
        if (rm.errorCode == 16 || rm.errorCode == 17) {
            timeOffsetMs_ = static_cast<int64_t>(msgId >> 32) * 1000 - host_->unixTimeMs();
            c.lastMsgId = 0;
            if (r) r->msgId = 0;
        } else if (rm.errorCode == 32 || rm.errorCode == 33) {
            host_->randomBytes(reinterpret_cast<uint8_t*>(&c.sessionId), 8);
            c.seq = 0;
            c.initSent = false;
            resendAll(c.dc);
        } else if (r) {
            finish(r->id, TlValue(), RpcErr(-1, "BAD_MSG_" + std::to_string(static_cast<long long>(rm.errorCode))));
        }
        break;
    }
    case RpcMessage::NewSessionCreated:
        key(c.dc).salt = rm.serverSalt;
        host_->onStateChanged();
        if (c.dc == homeDc_) host_->onSessionReset();
        break;
    default:
        break;
    }
}

void Engine::handleResult(Conn& c, uint64_t reqMsgId, const std::vector<uint8_t>& rawIn)
{
    Req* r = findByMsg(reqMsgId);
    if (!r) return;
    std::vector<uint8_t> raw = rawIn;
    if (raw.size() >= 4 && le32(&raw[0]) == kGzipPackedId) {
        TlReader rd(&raw[0], raw.size());
        rd.readConstructor();
        std::string packed = rd.readBytes(), plain;
        if (!gunzipBytes(packed, plain)) { finish(r->id, TlValue(), RpcErr(-1, "GZIP")); return; }
        raw.assign(plain.begin(), plain.end());
    }
    if (raw.size() >= 4 && le32(&raw[0]) == kRpcErrorId) {
        TlReader rd(&raw[0], raw.size());
        rd.readConstructor();
        int code = rd.readInt32();
        std::string msg = rd.readString();
        handleError(*r, RpcErr(code, msg));
        return;
    }
    TlValue v;
    try {
        v = tlParseResult(r->fn, raw.empty() ? 0 : &raw[0], raw.size());
    } catch (const std::exception& e) {
        log(std::string("result parse (") + r->fn.name() + "): " + e.what());
        finish(r->id, TlValue(), RpcErr(-1, std::string("PARSE: ") + e.what()));
        return;
    }
    finish(r->id, v, RpcErr());
}

void Engine::handleError(Req& r, const RpcErr& e)
{
    std::string prefix;
    int n = 0;
    bool numbered = splitNumber(e.message, prefix, n);
    int64_t t = now();
    if (e.code == 303 && numbered && n > 0) {
        if (prefix == "PHONE_MIGRATE_" || prefix == "USER_MIGRATE_" || prefix == "NETWORK_MIGRATE_") {
            log("home DC -> " + std::to_string(static_cast<long long>(n)));
            if (!authorized_) homeDc_ = n;
            host_->onStateChanged();
        }
        r.dc = n;
        r.msgId = 0;
        Conn& c = conn(n);
        if (c.st == Conn::Ready) pump(c);
        else if (c.st == Conn::Idle) connectDc(n);
        return;
    }
    if (e.code == 420 && numbered && n <= 60 && r.tries < 5) {
        r.notBeforeMs = t + n * 1000LL + 500;
        r.msgId = 0;
        return;
    }
    if ((e.code == 500 || e.code == -503) && r.tries < 3) {
        r.notBeforeMs = t + 1000;
        r.msgId = 0;
        return;
    }
    if (e.code == 401 && r.dc != homeDc_ && r.tries < 3 && authorized_) {
        key(r.dc).imported = false;
        r.msgId = 0;
        startImport(r.dc);
        return;
    }
    if (e.code == 401 && r.dc == homeDc_ && authorized_ &&
        (e.message == "AUTH_KEY_UNREGISTERED" || e.message == "SESSION_REVOKED" || e.message == "USER_DEACTIVATED")) {
        authorized_ = false;
        host_->onStateChanged();
        host_->onAuthLost();
    }
    finish(r.id, TlValue(), e);
}

} // namespace tg
