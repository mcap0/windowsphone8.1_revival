#include "tailnet.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace ts {

static const unsigned kCapVer = 148;

static uint32_t parseIp4(const std::string& s) {
    unsigned a, b, c, d;
    if (sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return a << 24 | b << 16 | c << 8 | d;
}

static std::string ip4str(uint32_t ip) {
    char b[20];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

static bool parseIpPort(const std::string& s, uint8_t ip[4], uint16_t& port) {
    unsigned a, b, c, d, p;
    if (sscanf(s.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) != 5) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255 || p > 65535) return false;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b; ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    port = (uint16_t)p;
    return true;
}

static bool parseKey(const std::string& s, const char* prefix, uint8_t out[32]) {
    size_t n = strlen(prefix);
    return s.compare(0, n, prefix) == 0 && unhex(s.substr(n), out, 32);
}

static uint32_t firstIp4(const Json& addrs) {
    for (size_t i = 0; i < addrs.size(); i++) {
        uint32_t ip = parseIp4(addrs[i].s());
        if (ip) return ip;
    }
    return 0;
}

Tailnet::Tailnet(const Options& o)
    : opt_(o), running_(false), status_("spento"), holePunch_(o.holePunch), epBusy_(false) {
    if (!opt_.log) opt_.log = [](const std::string&) {};
    if (opt_.hostname.empty()) opt_.hostname = "lumia";
}

Tailnet::~Tailnet() { stop(); }

void Tailnet::setStatus(const std::string& s) {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (status_ == s) return;
        status_ = s;
    }
    opt_.log("tailnet: " + s);
}

std::string Tailnet::status() {
    std::lock_guard<std::mutex> l(mu_);
    return status_;
}

bool Tailnet::ready() {
    std::lock_guard<std::mutex> l(mu_);
    return selfIp_ != 0 && !peers_.empty();
}

bool Tailnet::loadState() {
    FILE* f = fopen(opt_.statePath.c_str(), "rb");
    if (f) {
        char line[256];
        uint32_t self = 0;
        std::map<std::string, Peer> peers;
        std::map<int, std::vector<std::string> > derps;
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "machine") unhex(v, machine_, 32);
            else if (k == "node") unhex(v, node_, 32);
            else if (k == "disco") unhex(v, disco_, 32);
            else if (k == "registered") registered_ = v == "1";
            else if (k == "derp") preferredDerp_ = atoi(v.c_str());
            else if (k == "self") self = parseIp4(v);
            else if (k == "peer") {
                char id[64], key[80], ip[24], host[80];
                int derp = 0;
                if (sscanf(v.c_str(), "%63s %79s %23s %d %79s", id, key, ip, &derp, host) != 5) continue;
                Peer p;
                p.id = id;
                p.host = host;
                p.ip = parseIp4(ip);
                p.derp = derp;
                if (unhex(key, p.key, 32)) peers[p.id] = p;
            } else if (k == "derphost") {
                int region = 0;
                char host[128];
                if (sscanf(v.c_str(), "%d %127s", &region, host) == 2) derps[region].push_back(host);
            }
        }
        fclose(f);
        x25519Base(nodePub_, node_);
        x25519Base(discoPub_, disco_);
        // last known map: dial() can start at once, control corrects it when it answers
        {
            std::lock_guard<std::mutex> l(mu_);
            if (self && peers_.empty()) {
                selfIp_ = self;
                peers_ = peers;
                derpHosts_ = derps;
            }
        }
        savedMap_ = mapLines();
        return true;
    }
    uint8_t pub[32];
    newKeyPair(machine_, pub);
    newKeyPair(node_, nodePub_);
    newKeyPair(disco_, discoPub_);
    registered_ = false;
    saveState();
    return true;
}

std::string Tailnet::mapLines() {
    std::lock_guard<std::mutex> l(mu_);
    if (!selfIp_) return std::string();
    std::string out = "self=" + ip4str(selfIp_) + "\n";
    std::set<int> regions;
    char b[32];
    for (std::map<std::string, Peer>::const_iterator it = peers_.begin(); it != peers_.end(); ++it) {
        const Peer& p = it->second;
        if (p.id.empty() || p.host.empty() || p.host.size() > 79 || p.id.size() > 63) continue;
        snprintf(b, sizeof b, " %d ", p.derp);
        out += "peer=" + p.id + " " + hex(p.key, 32) + " " + ip4str(p.ip) + b + p.host + "\n";
        if (p.derp) regions.insert(p.derp);
    }
    for (std::set<int>::const_iterator r = regions.begin(); r != regions.end(); ++r) {
        std::map<int, std::vector<std::string> >::const_iterator h = derpHosts_.find(*r);
        if (h == derpHosts_.end()) continue;
        for (size_t i = 0; i < h->second.size(); i++) {
            snprintf(b, sizeof b, "%d ", *r);
            out += "derphost=" + (b + h->second[i]) + "\n";
        }
    }
    return out;
}

void Tailnet::saveMap() {
    std::string m = mapLines();
    {
        std::lock_guard<std::mutex> l(saveMu_);
        if (m == savedMap_) return;
    }
    saveState();
}

void Tailnet::saveState() {
    std::string map = mapLines();
    std::lock_guard<std::mutex> l(saveMu_);
    savedMap_ = map;
    std::string tmp = opt_.statePath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "machine=%s\nnode=%s\ndisco=%s\nregistered=%d\nderp=%d\n%s", hex(machine_, 32).c_str(),
            hex(node_, 32).c_str(), hex(disco_, 32).c_str(), registered_ ? 1 : 0, preferredDerp_, map.c_str());
    fclose(f);
    remove(opt_.statePath.c_str());
    rename(tmp.c_str(), opt_.statePath.c_str());
}

std::string Tailnet::hostinfo() {
    std::string h = "{\"IPNVersion\":\"1.104.2-lumia\",\"OS\":\"windows\",\"OSVersion\":\"Windows Phone 8.1\","
                    "\"DeviceModel\":\"Lumia 1020\",\"Hostname\":" + jsonQuote(opt_.hostname);
    if (preferredDerp_) {
        char b[48];
        snprintf(b, sizeof b, ",\"NetInfo\":{\"PreferredDERP\":%d}", preferredDerp_);
        h += b;
    }
    return h + "}";
}

std::string Tailnet::mapRequest(bool stream) {
    char v[16];
    snprintf(v, sizeof v, "%u", kCapVer);
    std::string r = std::string("{\"Version\":") + v + ",\"NodeKey\":\"nodekey:" + hex(nodePub_, 32) +
                    "\",\"DiscoKey\":\"discokey:" + hex(discoPub_, 32) + "\",\"Hostinfo\":" + hostinfo() +
                    endpointsJson();
    if (stream) r += ",\"Stream\":true,\"KeepAlive\":true";
    else r += ",\"OmitPeers\":true";   // "lite" update: just tells control our new home DERP
    return r + "}";
}

bool Tailnet::registerNode(Control& c) {
    char v[16];
    snprintf(v, sizeof v, "%u", kCapVer);
    std::string base = std::string("{\"Version\":") + v + ",\"NodeKey\":\"nodekey:" + hex(nodePub_, 32) +
                       "\",\"Hostinfo\":" + hostinfo();
    if (!opt_.authKey.empty()) base += ",\"Auth\":{\"AuthKey\":" + jsonQuote(opt_.authKey) + "}";
    std::string followup;
    for (int round = 0; round < 3 && running_; round++) {
        std::string req = base + (followup.empty() ? "" : ",\"Followup\":" + jsonQuote(followup)) + "}";
        unsigned id = c.post("/machine/register", req);
        if (!id) return false;
        // with a Followup the server answers once the link has been opened (up to some minutes)
        std::string body = c.readAll(id, followup.empty() ? 30000 : 15 * 60000);
        int st = c.status(id, 1000);
        c.forget(id);
        Json r;
        if (st != 200 || !Json::parse(body.data(), body.size(), r)) {
            setStatus("registrazione rifiutata: " + body.substr(0, 200));
            return false;
        }
        if (!r["Error"].s().empty()) {
            setStatus("registrazione: " + r["Error"].s());
            return false;
        }
        if (r["MachineAuthorized"].b) {
            registered_ = true;
            saveState();
            setStatus("registrato");
            return true;
        }
        std::string url = r["AuthURL"].s();
        if (url.empty()) {
            setStatus("registrazione: in attesa di approvazione");
            return false;
        }
        followup = url;
        setStatus("serve il login: " + url);
        if (opt_.onAuthUrl) opt_.onAuthUrl(url);
    }
    return false;
}

void Tailnet::applyMap(const Json& m) {
    std::lock_guard<std::mutex> l(mu_);
    if (m.has("Node")) {
        uint32_t ip = firstIp4(m["Node"]["Addresses"]);
        if (ip) selfIp_ = ip;
    }
    const Json& regions = m["DERPMap"]["Regions"];
    if (regions.type == Json::Obj) {
        derpHosts_.clear();
        for (std::map<std::string, Json>::const_iterator it = regions.obj.begin(); it != regions.obj.end(); ++it) {
            int id = (int)it->second["RegionID"].i();
            const Json& nodes = it->second["Nodes"];
            for (size_t i = 0; i < nodes.size(); i++)
                if (!nodes[i]["STUNOnly"].b && !nodes[i]["HostName"].s().empty())
                    derpHosts_[id].push_back(nodes[i]["HostName"].s());
        }
        std::map<int, std::vector<std::string> > stun;
        for (std::map<std::string, Json>::const_iterator it = regions.obj.begin(); it != regions.obj.end(); ++it) {
            int id = (int)it->second["RegionID"].i();
            const Json& nodes = it->second["Nodes"];
            for (size_t i = 0; i < nodes.size(); i++) {
                long long port = nodes[i].has("STUNPort") ? nodes[i]["STUNPort"].i() : 0;
                if (port < 0) continue;   // STUN off on this node
                uint32_t ip = parseIp4(nodes[i]["IPv4"].s());
                if (ip) stun[id].push_back(ip4str(ip) + ":" + std::to_string(port ? port : 3478));
            }
        }
        std::lock_guard<std::mutex> u(udpMu_);
        stunHosts_.swap(stun);
    }
    struct Upsert {
        static void node(std::map<std::string, Peer>& peers, const Json& n) {
            Peer p;
            p.id = n["ID"].s();
            if (!parseKey(n["Key"].s(), "nodekey:", p.key)) return;
            p.ip = firstIp4(n["Addresses"]);
            p.host = n["Hostinfo"]["Hostname"].s();
            std::string name = n["Name"].s();
            if (!name.empty()) p.host = name.substr(0, name.find('.'));   // MagicDNS name = what the console shows
            p.derp = (int)n["HomeDERP"].i();
            std::string legacy = n["DERP"].s();   // "127.3.3.40:<region>"
            if (!p.derp && legacy.compare(0, 11, "127.3.3.40:") == 0) p.derp = atoi(legacy.c_str() + 11);
            p.hasDisco = parseKey(n["DiscoKey"].s(), "discokey:", p.disco);
            const Json& eps = n["Endpoints"];
            for (size_t i = 0; i < eps.size(); i++) p.endpoints.push_back(eps[i].s());
            peers[p.id] = p;
        }
    };
    if (m.has("Peers")) {
        peers_.clear();
        for (size_t i = 0; i < m["Peers"].size(); i++) Upsert::node(peers_, m["Peers"][i]);
    }
    for (size_t i = 0; i < m["PeersChanged"].size(); i++) Upsert::node(peers_, m["PeersChanged"][i]);
    for (size_t i = 0; i < m["PeersRemoved"].size(); i++) peers_.erase(m["PeersRemoved"][i].s());
    const Json& patches = m["PeersChangedPatch"];
    for (size_t i = 0; i < patches.size(); i++) {
        std::map<std::string, Peer>::iterator it = peers_.find(patches[i]["NodeID"].s());
        if (it == peers_.end()) continue;
        if (patches[i].has("DERPRegion")) it->second.derp = (int)patches[i]["DERPRegion"].i();
        if (patches[i].has("Key")) parseKey(patches[i]["Key"].s(), "nodekey:", it->second.key);
        if (patches[i].has("DiscoKey")) it->second.hasDisco = parseKey(patches[i]["DiscoKey"].s(), "discokey:", it->second.disco);
        if (patches[i].has("Endpoints")) {
            it->second.endpoints.clear();
            const Json& eps = patches[i]["Endpoints"];
            for (size_t j = 0; j < eps.size(); j++) it->second.endpoints.push_back(eps[j].s());
        }
    }
}

void Tailnet::start() {
    if (running_) return;
    running_ = true;   // the state is read by run(): the keys cost milliseconds of curve math, not the caller's
    openUdp();
    runThread_ = std::thread(&Tailnet::run, this);
    tickThread_ = std::thread(&Tailnet::ticker, this);
}

void Tailnet::stop() {
    if (!running_) return;
    running_ = false;
    std::shared_ptr<Control> c;
    std::shared_ptr<Derp> d;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = ctrl_;
    }
    {
        std::lock_guard<std::mutex> l(derpMu_);
        d = derp_;
        derp_.reset();
    }
    if (c) c->close();
    if (d) d->close();
    closeUdp();
    if (runThread_.joinable()) runThread_.join();
    if (tickThread_.joinable()) tickThread_.join();
    if (epThread_.joinable()) epThread_.join();
    std::vector<std::weak_ptr<TcpConn> > conns;
    {
        std::lock_guard<std::mutex> l(mu_);
        conns.swap(conns_);
        wg_.clear();
        ctrl_.reset();
    }
    for (size_t i = 0; i < conns.size(); i++)
        if (std::shared_ptr<TcpConn> t = conns[i].lock()) t->close();
    setStatus("spento");
}

void Tailnet::kick() {
    std::shared_ptr<Control> c;
    std::shared_ptr<Derp> d;
    std::vector<std::weak_ptr<TcpConn> > conns;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = ctrl_;
        // the background task may have handshaken with the same key meanwhile: the old sessions are dead
        wg_.clear();
        conns.swap(conns_);
    }
    for (size_t i = 0; i < conns.size(); i++)
        if (std::shared_ptr<TcpConn> t = conns[i].lock()) t->close();
    {
        std::lock_guard<std::mutex> l(derpMu_);
        d = derp_;
        derp_.reset();
    }
    if (c) c->close();
    if (d) d->close();
    closeUdp();
    if (running_) openUdp();   // a new source port: after a suspend the old NAT mapping is dead
}

void Tailnet::run() {
    loadState();
    {   // the names once, even with hole punching off (it can be turned on later); the ticker must not wait for DNS
        std::vector<std::string> pub;
        std::string a = resolve4("stun.l.google.com", 19302), b = resolve4("stun.cloudflare.com", 3478);
        if (!a.empty()) pub.push_back(a);
        if (!b.empty()) pub.push_back(b);
        std::lock_guard<std::mutex> l(udpMu_);
        stunPublic_ = pub;
    }
    int backoff = 2000;
    while (running_) {
        std::shared_ptr<Control> c(new Control(opt_.log));
        std::string err;
        setStatus("collegamento al controllo");
        if (!c->connect(machine_, err)) {
            setStatus(err);
        } else {
            {
                std::lock_guard<std::mutex> l(mu_);
                ctrl_ = c;
            }
            derpAnnounced_ = false;   // a new control session: the home DERP is told again (announceDerp)
            if (registered_ || registerNode(*c)) {
                unsigned id = c->post("/machine/map", mapRequest(true));
                int st = id ? c->status(id, 30000) : -1;
                if (st == 401 || st == 403) {   // node key expired or removed from the admin console
                    registered_ = false;
                    saveState();
                    setStatus("nodo non più valido: nuova registrazione");
                } else if (st == 200) {
                    backoff = 2000;
                    std::string buf;
                    while (running_ && c->alive()) {
                        uint8_t sz[4];
                        if (!c->readBodyFull(id, sz, 4, 150000)) break;   // keepalives arrive every minute
                        uint32_t n = sz[0] | sz[1] << 8 | sz[2] << 16 | (uint32_t)sz[3] << 24;
                        if (n > (64u << 20)) break;
                        buf.resize(n);
                        if (n && !c->readBodyFull(id, (uint8_t*)&buf[0], n, 60000)) break;
                        Json m;
                        if (!Json::parse(buf.data(), buf.size(), m)) continue;
                        applyMap(m);
                        if (ready()) {
                            {
                                std::lock_guard<std::mutex> l(mu_);
                                status_ = "connesso come " + ip4str(selfIp_);
                            }
                            saveMap();
                        }
                    }
                } else {
                    setStatus("mappa rifiutata");
                }
            }
            c->close();
            {
                std::lock_guard<std::mutex> l(mu_);
                ctrl_.reset();
            }
        }
        for (int t = 0; t < backoff && running_; t += 100) sleepMs(100);
        backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
    }
}

void Tailnet::ticker() {
    while (running_) {
        sleepMs(50);
        std::vector<std::shared_ptr<WgPeer> > peers;
        std::vector<std::shared_ptr<TcpConn> > conns;
        {
            std::lock_guard<std::mutex> l(mu_);
            for (std::map<std::string, std::shared_ptr<WgPeer> >::iterator it = wg_.begin(); it != wg_.end(); ++it)
                peers.push_back(it->second);
            for (size_t i = 0; i < conns_.size();) {
                std::shared_ptr<TcpConn> t = conns_[i].lock();
                if (!t) { conns_.erase(conns_.begin() + i); continue; }
                conns.push_back(t);
                i++;
            }
        }
        for (size_t i = 0; i < peers.size(); i++) peers[i]->tick();
        for (size_t i = 0; i < conns.size(); i++) conns[i]->tick();
        discoTick();
    }
}

bool Tailnet::ensureDerp(int region, std::string& err) {
    std::lock_guard<std::mutex> l(derpMu_);
    if (derp_ && derp_->alive() && derpRegion_ == region) {
        announceDerp(region);
        return true;
    }
    if (derp_) derp_->close();
    derp_.reset();
    std::vector<std::string> hosts;
    {
        std::lock_guard<std::mutex> l2(mu_);
        hosts = derpHosts_[region];
    }
    if (hosts.empty()) {
        err = "regione DERP sconosciuta";
        return false;
    }
    for (size_t i = 0; i < hosts.size(); i++) {
        std::shared_ptr<Derp> d(new Derp(opt_.log, [this](const uint8_t s[32], const uint8_t* p, size_t n) { onDerp(s, p, n); }));
        if (d->connect(hosts[i], node_, err)) {
            derp_ = d;
            derpRegion_ = region;
            opt_.log("DERP " + hosts[i]);
            break;
        }
    }
    if (!derp_) return false;
    announceDerp(region);
    return true;
}

// Tells control (and so the peers) our home DERP, the only way their replies reach us (no direct UDP). Once per
// control connection, not only when the region changes: the region saved in the state from an earlier run made
// it skip the update forever, the hostinfo of the streaming map request alone was not taken, and the peers had
// no relay for us (Remoto 2026-11-07: "lumia-remoto" with Relay "" on the Pixel, every dial timed out).
void Tailnet::announceDerp(int region) {
    if (preferredDerp_ == region && derpAnnounced_) return;
    if (preferredDerp_ != region) {
        preferredDerp_ = region;
        saveState();
    }
    std::shared_ptr<Control> c;
    {
        std::lock_guard<std::mutex> l2(mu_);
        c = ctrl_;
    }
    if (!c) return;
    unsigned id = c->post("/machine/map", mapRequest(false));
    if (id) {
        if (c->status(id, 10000) == 200) derpAnnounced_ = true;
        c->forget(id);
    }
    opt_.log(std::string("DERP ") + (derpAnnounced_ ? "annunciato" : "non annunciato") + " (regione " + std::to_string(region) + ")");
}

WgPeer* Tailnet::peerFor(const uint8_t key[32]) {
    std::string k = hex(key, 32);
    std::map<std::string, std::shared_ptr<WgPeer> >::iterator it = wg_.find(k);
    if (it != wg_.end()) return it->second.get();
    std::string dst((const char*)key, 32);   // VS2013 lambdas cannot capture arrays
    std::shared_ptr<WgPeer> p(new WgPeer(node_, key,
        [this, dst, k](const uint8_t* m, size_t n) {
            if (sendDirect(k, m, n)) return;   // a proven direct UDP path
            std::shared_ptr<Derp> d;
            {
                std::lock_guard<std::mutex> l(derpMu_);
                d = derp_;
            }
            if (d) d->send((const uint8_t*)dst.data(), m, n);
        },
        [this](const uint8_t* ip, size_t n) {
            std::shared_ptr<TcpConn> target;
            {
                std::lock_guard<std::mutex> l(mu_);
                for (size_t i = 0; i < conns_.size() && !target; i++) {
                    std::shared_ptr<TcpConn> t = conns_[i].lock();
                    if (t && t->matches(ip, n)) target = t;
                }
            }
            if (target) target->input(ip, n);
        }));
    wg_[k] = p;
    return p.get();
}

void Tailnet::onDerp(const uint8_t src[32], const uint8_t* p, size_t n) {
    if (n >= 6 && memcmp(p, "TS\xf0\x9f\x92\xac", 6) == 0) {   // disco over DERP: only CallMeMaybe is used
        if (holePunch_) onDiscoDerp(src, p, n);
        return;
    }
    std::shared_ptr<WgPeer> peer;
    {
        std::lock_guard<std::mutex> l(mu_);
        bool known = false;
        for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end() && !known; ++it)
            known = memcmp(it->second.key, src, 32) == 0;
        if (!known) return;
        peerFor(src);
        peer = wg_[hex(src, 32)];
    }
    peer->receive(p, n);
}

// ---------------------------------------------------------------------------------- direct UDP path
//
// DERP relays every packet through Frankfurt (measured: 46 ms one way, TCP head-of-line behind it). The Pixel
// advertises its endpoints in the netmap, so we can reach it directly with UDP: a disco ping teaches it our
// source address, its own disco pings then promote that address to its best path, and WireGuard runs over the
// same socket. DERP stays as the fallback for peers with no endpoints and while the path is unproven.

bool Tailnet::openUdp() {
    if (bench.noUdp) return false;
    std::shared_ptr<Udp> u(new Udp([this](const uint8_t* p, size_t n, const std::string& from) { onUdp(p, n, from); }));
    {
        std::lock_guard<std::mutex> l(udpMu_);
        udp_ = u;
    }
    std::string err;
    if (!u->open(err)) {
        {
            std::lock_guard<std::mutex> l(udpMu_);
            udp_.reset();
        }
        opt_.log("tailnet: " + err + " (solo DERP)");
        return false;
    }
    opt_.log("tailnet: UDP diretto " + u->local());
    udpSince_ = nowMs();
    return true;
}

void Tailnet::closeUdp() {
    std::shared_ptr<Udp> u;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        u = udp_;
        udp_.reset();
        addrKey_.clear();
        direct_.clear();
        udpSeen_.clear();
        pingSent_.clear();
        stunAddr_.clear();   // a new socket has a new public port
        stunAddr2_.clear();
        natKind_.clear();
        stunSent_ = stunGot_ = 0;
        cmmSent_.clear();
        punchUntil_.clear();
    }
    if (u) u->close();
}

void Tailnet::markDirect(const std::string& keyHex, const std::string& from) {
    bool changed;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        std::map<std::string, std::string>::iterator it = direct_.find(keyHex);
        changed = it == direct_.end() || it->second != from;
        direct_[keyHex] = from;
        udpSeen_[keyHex] = nowMs();
        addrKey_[from] = keyHex;
    }
    if (changed) opt_.log("tailnet: percorso diretto " + from);
}

// A disco ping: "TS💬" + our disco public key + nacl box(ping). The peer learns our UDP source from it.
void Tailnet::sendPing(const Peer& p, const std::string& endpoint) {
    if (!p.hasDisco) return;
    std::shared_ptr<Udp> u;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        u = udp_;
    }
    if (!u) return;
    uint8_t inner[2 + 12 + 32];
    inner[0] = 1;   // disco.TypePing
    inner[1] = 0;   // version
    randomBytes(inner + 2, 12);   // TxID
    memcpy(inner + 14, nodePub_, 32);
    Bytes box = boxSeal(p.disco, disco_, inner, sizeof inner);
    if (box.empty()) return;
    static const char kMagic[] = "TS\xf0\x9f\x92\xac";
    std::vector<uint8_t> msg;
    msg.reserve(6 + 32 + box.size());
    msg.insert(msg.end(), (const uint8_t*)kMagic, (const uint8_t*)kMagic + 6);
    msg.insert(msg.end(), discoPub_, discoPub_ + 32);
    msg.insert(msg.end(), box.begin(), box.end());
    u->sendTo(endpoint, &msg[0], msg.size());
}

void Tailnet::onUdp(const uint8_t* p, size_t n, const std::string& from) {
    static const char kMagic[] = "TS\xf0\x9f\x92\xac";
    if (onStun(p, n)) return;
    if (n > 38 && memcmp(p, kMagic, 6) == 0) {
        const uint8_t* sender = p + 6;   // the sender's disco public key
        std::string keyHex;
        {
            std::lock_guard<std::mutex> l(mu_);
            for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end(); ++it)
                if (it->second.hasDisco && memcmp(it->second.disco, sender, 32) == 0) {
                    keyHex = hex(it->second.key, 32);
                    break;
                }
        }
        if (keyHex.empty()) return;   // a peer we do not know
        Bytes plain;
        if (!boxOpen(sender, disco_, p + 38, n - 38, plain) || plain.size() < 2) return;
        markDirect(keyHex, from);
        if (plain[0] != 1) return;   // ping; a pong only confirms the path (markDirect above)
        uint8_t ip[4];
        uint16_t port;
        if (!parseIpPort(from, ip, port)) return;
        std::vector<uint8_t> inner(2 + 12 + 18, 0);
        inner[0] = 2;   // disco.TypePong
        inner[1] = 0;
        memcpy(&inner[2], &plain[2], 12);   // TxID
        inner[14 + 10] = 0xff;              // v4-mapped IPv6, as Go's netip.Addr.As16
        inner[14 + 11] = 0xff;
        memcpy(&inner[14 + 12], ip, 4);
        inner[14 + 16] = (uint8_t)(port >> 8);
        inner[14 + 17] = (uint8_t)port;
        Bytes box = boxSeal(sender, disco_, &inner[0], inner.size());
        if (box.empty()) return;
        std::vector<uint8_t> msg;
        msg.reserve(6 + 32 + box.size());
        msg.insert(msg.end(), (const uint8_t*)kMagic, (const uint8_t*)kMagic + 6);
        msg.insert(msg.end(), discoPub_, discoPub_ + 32);
        msg.insert(msg.end(), box.begin(), box.end());
        std::shared_ptr<Udp> u;
        {
            std::lock_guard<std::mutex> l(udpMu_);
            u = udp_;
        }
        if (u) u->sendTo(from, &msg[0], msg.size());
        return;
    }
    if (n < 4 || p[0] < 1 || p[0] > 4) return;   // WireGuard (1 init, 2 response, 4 data)
    std::string keyHex;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        std::map<std::string, std::string>::iterator it = addrKey_.find(from);
        if (it != addrKey_.end()) keyHex = it->second;
    }
    std::shared_ptr<WgPeer> wg;
    if (!keyHex.empty()) {
        std::lock_guard<std::mutex> l(mu_);
        std::map<std::string, std::shared_ptr<WgPeer> >::iterator it = wg_.find(keyHex);
        if (it != wg_.end()) wg = it->second;
    }
    if (wg && wg->receive(p, n)) {
        markDirect(keyHex, from);
        return;
    }
    // Unknown source (or a session the mapped peer did not take): try every peer. A handshake identifies its owner.
    std::vector<std::pair<std::string, std::shared_ptr<WgPeer> > > all;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (std::map<std::string, std::shared_ptr<WgPeer> >::iterator it = wg_.begin(); it != wg_.end(); ++it)
            all.push_back(std::make_pair(it->first, it->second));
    }
    for (size_t i = 0; i < all.size(); i++)
        if (all[i].second->receive(p, n)) {
            markDirect(all[i].first, from);
            return;
        }
}

// WireGuard send: true = handed to the proven UDP path; false = the caller must also use DERP.
bool Tailnet::sendDirect(const std::string& keyHex, const uint8_t* p, size_t n) {
    std::shared_ptr<Udp> u;
    std::string proven;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        u = udp_;
        std::map<std::string, std::string>::iterator d = direct_.find(keyHex);
        std::map<std::string, int64_t>::iterator s = udpSeen_.find(keyHex);
        if (d != direct_.end() && s != udpSeen_.end() && nowMs() - s->second < 10000) {
            proven = d->second;
            fresh = true;
        }
    }
    if (!u || !fresh) return false;   // unproven: DERP only (tailscaled drops WireGuard from an address no disco
                                      // ping taught it, so sending data there was only wasted uplink)
    return u->sendTo(proven, p, n);
}

void Tailnet::pingPeer(const std::string& keyHex) {
    Peer p;
    bool found = false;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end(); ++it)
            if (hex(it->second.key, 32) == keyHex) {
                p = it->second;
                found = true;
                break;
            }
    }
    if (!found) return;
    std::vector<std::string> t = pingTargets(keyHex, p);
    for (size_t i = 0; i < t.size(); i++) sendPing(p, t[i]);
}

std::vector<std::string> Tailnet::pingTargets(const std::string& keyHex, const Peer& p) {
    std::vector<std::string> t = p.endpoints;
    if (!holePunch_) return t;
    std::lock_guard<std::mutex> l(udpMu_);
    std::map<std::string, std::vector<std::string> >::iterator c = cmmEps_.find(keyHex);
    if (c != cmmEps_.end())
        for (size_t i = 0; i < c->second.size(); i++)
            if (std::find(t.begin(), t.end(), c->second[i]) == t.end()) t.push_back(c->second[i]);
    return t;
}

void Tailnet::discoTick() {
    stunTick();
    bool punch = holePunch_;
    std::vector<std::pair<std::string, Peer> > list;
    std::set<std::string> talking;   // peers with a WireGuard session object: the ones we dial
    std::set<std::string> withCmm;
    if (punch) {
        std::lock_guard<std::mutex> l(udpMu_);
        for (std::map<std::string, std::vector<std::string> >::iterator it = cmmEps_.begin(); it != cmmEps_.end(); ++it)
            withCmm.insert(it->first);
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end(); ++it) {
            std::string k = hex(it->second.key, 32);
            if (it->second.hasDisco && (!it->second.endpoints.empty() || withCmm.count(k)))
                list.push_back(std::make_pair(k, it->second));
        }
        for (std::map<std::string, std::shared_ptr<WgPeer> >::iterator it = wg_.begin(); it != wg_.end(); ++it)
            talking.insert(it->first);
    }
    int64_t now = nowMs();
    for (size_t i = 0; i < list.size(); i++) {
        const std::string& k = list[i].first;
        int64_t last = 0;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> l(udpMu_);
            std::map<std::string, int64_t>::iterator s = pingSent_.find(k);
            if (s != pingSent_.end()) last = s->second;
            std::map<std::string, std::string>::iterator d = direct_.find(k);
            std::map<std::string, int64_t>::iterator seen = udpSeen_.find(k);
            fresh = d != direct_.end() && seen != udpSeen_.end() && now - seen->second < 10000;
        }
        // keep the NAT mapping alive once the path works; while it does not, probe every second for half a minute
        // after the netmap changed, then every 5 s (a carrier NAT that never lets us through costs little)
        bool punching = false, cmmDue = false;
        if (punch) {
            std::lock_guard<std::mutex> l(udpMu_);
            std::map<std::string, int64_t>::iterator pu = punchUntil_.find(k);
            punching = pu != punchUntil_.end() && now < pu->second;
            // CallMeMaybe while the path is not proven: every 5 s for the first 6, then every 30 s
            if (fresh) cmmCount_[k] = 0;
            else if (talking.count(k) && !stunAddr_.empty()) {
                std::map<std::string, int64_t>::iterator cs = cmmSent_.find(k);
                int64_t every = cmmCount_[k] < 6 ? 5000 : 30000;
                cmmDue = cs == cmmSent_.end() || now - cs->second >= every;
            }
        }
        if (cmmDue) sendCallMeMaybe(list[i].second);   // pings its endpoints too
        int64_t interval = fresh ? 15000 : punching ? 500 : (now - udpSince_ < 30000 ? 1000 : 5000);
        if (last && now - last < interval) continue;
        {
            std::lock_guard<std::mutex> l(udpMu_);
            pingSent_[k] = now;
        }
        std::vector<std::string> t = pingTargets(k, list[i].second);
        for (size_t j = 0; j < t.size(); j++) sendPing(list[i].second, t[j]);
    }
}

// ---------------------------------------------------------------------------------- hole punching
//
// The Lumia on 4G and the Pixel at home are both behind a NAT that drops what arrives unasked. STUN (a Binding
// request to the DERP region's STUN server) tells us the public address our UDP socket has; we put it in the
// netmap (Endpoints) and send it straight to the peer in a disco CallMeMaybe over DERP. The peer then pings that
// address while we ping its addresses: each side's outgoing ping opens its own NAT for the other's, and the first
// ping that gets through is answered with a pong, which proves the path (markDirect). If the carrier NAT changes the
// port per destination it may never work: everything stays on DERP as without hole punching.

void Tailnet::setHolePunch(bool on) {
    if (holePunch_.exchange(on) == on) return;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        cmmEps_.clear();
        cmmSent_.clear();
        cmmCount_.clear();
        punchUntil_.clear();
        stunSent_ = 0;
        epTried_ = 0;
    }
    opt_.log(std::string("tailnet: hole punching ") + (on ? "acceso" : "spento"));
}

std::string Tailnet::pathTo(const std::string& host) {
    std::string k;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end(); ++it)
            if (it->second.host == host) k = hex(it->second.key, 32);
    }
    std::lock_guard<std::mutex> l(udpMu_);
    std::map<std::string, std::string>::iterator d = direct_.find(k);
    std::map<std::string, int64_t>::iterator s = udpSeen_.find(k);
    if (d != direct_.end() && s != udpSeen_.end() && nowMs() - s->second < 10000) return "diretto " + d->second;
    return "relay";
}

std::string Tailnet::natKind() {
    std::lock_guard<std::mutex> l(udpMu_);
    return natKind_;
}

static const uint32_t kStunCookie = 0x2112A442;

void Tailnet::stunTick() {
    int64_t now = nowMs();
    bool punch = holePunch_;
    std::shared_ptr<Udp> u;
    std::string server, server2, desired;
    bool announce = false;
    int region;
    {
        std::lock_guard<std::mutex> l(mu_);
        region = derpRegion_ ? derpRegion_ : preferredDerp_;
    }
    {
        std::lock_guard<std::mutex> l(udpMu_);
        u = udp_;
        if (punch && u) {
            int64_t every = stunAddr_.empty() ? 3000 : 25000;   // also keeps the carrier's mapping alive
            if (!stunSent_ || now - stunSent_ >= every) {
                // Tailscale's own STUN (port 3478 on the DERP nodes) did not answer from home (2026-11-08), Google's and
                // Cloudflare's did: those first, then the home region's
                std::vector<std::string> all = stunPublic_;
                std::map<int, std::vector<std::string> >::iterator h = stunHosts_.find(region);
                if (h == stunHosts_.end() || h->second.empty()) h = stunHosts_.begin();
                if (h != stunHosts_.end()) all.insert(all.end(), h->second.begin(), h->second.end());
                if (!all.empty()) {
                    if (stunSent_ && stunGot_ < stunSent_) stunNext_++;   // the last one never answered: next server
                    server = all[stunNext_ % all.size()];
                    if (all.size() > 1) server2 = all[(stunNext_ + 1) % all.size()];
                    randomBytes(stunTx_, 12);
                    randomBytes(stunTx2_, 12);
                    stunSent_ = now;
                }
            }
        }
        desired = punch ? stunAddr_ : "";
        // on with no STUN answer yet (new socket after a suspend): wait for it instead of withdrawing the old address
        announce = desired != epSent_ && !(punch && desired.empty()) && !epBusy_ && now - epTried_ >= 10000;
        if (announce) epTried_ = now;
    }
    if (!server.empty()) {
        uint8_t req[20] = {0, 1, 0, 0};   // Binding request, no attributes
        req[4] = kStunCookie >> 24; req[5] = (kStunCookie >> 16) & 255; req[6] = (kStunCookie >> 8) & 255; req[7] = kStunCookie & 255;
        uint8_t req2[20];
        memcpy(req2, req, 8);
        {
            std::lock_guard<std::mutex> l(udpMu_);
            memcpy(req + 8, stunTx_, 12);
            memcpy(req2 + 8, stunTx2_, 12);
        }
        u->sendTo(server, req, sizeof req);
        if (!server2.empty()) u->sendTo(server2, req2, sizeof req2);
    }
    if (announce) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (!ctrl_) return;
        }
        if (epThread_.joinable()) epThread_.join();   // finished (epBusy_ is false)
        epBusy_ = true;
        epThread_ = std::thread([this]() { announceEndpoints(); epBusy_ = false; });
    }
}

bool Tailnet::onStun(const uint8_t* p, size_t n) {
    if (n < 20 || p[0] != 0x01 || p[1] != 0x01) return false;   // Binding success response
    uint32_t cookie = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
    if (cookie != kStunCookie) return false;
    std::string addr;
    bool second;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        second = memcmp(p + 8, stunTx2_, 12) == 0;
        if (!second && memcmp(p + 8, stunTx_, 12) != 0) return true;   // an old or foreign answer
    }
    size_t len = (size_t)(p[2] << 8 | p[3]);
    if (20 + len > n) return true;
    for (size_t o = 20; o + 4 <= 20 + len;) {
        unsigned type = p[o] << 8 | p[o + 1], alen = p[o + 2] << 8 | p[o + 3];
        const uint8_t* v = p + o + 4;
        if (o + 4 + alen > 20 + len) break;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 1) {   // (XOR-)MAPPED-ADDRESS, IPv4
            uint16_t port = (uint16_t)(v[2] << 8 | v[3]);
            uint32_t ip = (uint32_t)v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7];
            if (type == 0x0020) {
                port ^= kStunCookie >> 16;
                ip ^= kStunCookie;
            }
            addr = ip4str(ip) + ":" + std::to_string(port);
            if (type == 0x0020) break;
        }
        o += 4 + ((alen + 3) & ~3u);
    }
    if (addr.empty()) return true;
    // Two servers, same socket: the same public port from both = the NAT keeps one mapping for every destination
    // (punching can work); different ports = a new mapping per destination (symmetric: the peer's pings go to a port
    // that is not ours toward it, so it cannot work without a relay)
    std::string kind, a1, a2;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        if (second) stunAddr2_ = addr;
        a1 = second ? stunAddr_ : addr;
        a2 = stunAddr2_;
        if (!a1.empty() && !a2.empty()) {
            kind = a1 == a2 ? "NAT stessa porta" : "NAT porta per destinazione";
            if (kind == natKind_) kind.clear();
            else natKind_ = kind;
        }
    }
    if (!kind.empty()) opt_.log("tailnet: " + kind + " (" + a1 + " / " + a2 + ")");
    if (second) return true;
    bool changed;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        changed = addr != stunAddr_;
        stunAddr_ = addr;
        stunGot_ = nowMs();
        if (changed) cmmSent_.clear();   // the peers get the new address at once
    }
    if (changed) opt_.log("tailnet: STUN " + addr);
    return true;
}

std::string Tailnet::endpointsJson() {
    std::lock_guard<std::mutex> l(udpMu_);
    std::string desired = holePunch_ ? stunAddr_ : "";
    // empty when off: what control had before hole punching existed (also clears an address left from a run with it on)
    if (desired.empty()) return ",\"Endpoints\":[],\"EndpointTypes\":[]";
    return ",\"Endpoints\":[" + jsonQuote(desired) + "],\"EndpointTypes\":[2]";   // 2 = found by STUN
}

// A "lite" map request with our endpoints (as tailscaled does when they change). Own thread: up to 10 s.
void Tailnet::announceEndpoints() {
    std::shared_ptr<Control> c;
    {
        std::lock_guard<std::mutex> l(mu_);
        c = ctrl_;
    }
    if (!c) return;
    std::string desired;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        desired = holePunch_ ? stunAddr_ : "";
    }
    unsigned id = c->post("/machine/map", mapRequest(false));
    bool ok = false;
    if (id) {
        ok = c->status(id, 10000) == 200;
        c->forget(id);
    }
    if (ok) {
        std::lock_guard<std::mutex> l(udpMu_);
        epSent_ = desired;
    }
    opt_.log("tailnet: indirizzi " + (desired.empty() ? std::string("ritirati") : desired) + (ok ? " annunciati" : " non annunciati"));
}

Bytes Tailnet::discoPacket(const uint8_t peerDisco[32], const std::vector<uint8_t>& inner) {
    Bytes box = boxSeal(peerDisco, disco_, &inner[0], inner.size());
    if (box.empty()) return box;
    static const char kMagic[] = "TS\xf0\x9f\x92\xac";
    Bytes msg;
    msg.reserve(6 + 32 + box.size());
    msg.insert(msg.end(), (const uint8_t*)kMagic, (const uint8_t*)kMagic + 6);
    msg.insert(msg.end(), discoPub_, discoPub_ + 32);
    msg.insert(msg.end(), box.begin(), box.end());
    return msg;
}

// disco.TypeCallMeMaybe over DERP: "my endpoints are these, ping them now". Then we ping the peer's at once.
void Tailnet::sendCallMeMaybe(const Peer& p) {
    if (!p.hasDisco) return;
    std::string k = hex(p.key, 32), addr;
    int count;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        addr = stunAddr_;
        if (addr.empty()) return;
        int64_t now = nowMs();
        cmmSent_[k] = now;
        count = ++cmmCount_[k];
        punchUntil_[k] = now + 10000;
    }
    uint8_t ip[4];
    uint16_t port;
    if (!parseIpPort(addr, ip, port)) return;
    std::vector<uint8_t> inner(2 + 18, 0);
    inner[0] = 3;   // disco.TypeCallMeMaybe, version 0
    inner[2 + 10] = 0xff;
    inner[2 + 11] = 0xff;
    memcpy(&inner[2 + 12], ip, 4);
    inner[2 + 16] = (uint8_t)(port >> 8);
    inner[2 + 17] = (uint8_t)port;
    Bytes msg = discoPacket(p.disco, inner);
    std::shared_ptr<Derp> d;
    {
        std::lock_guard<std::mutex> l(derpMu_);
        d = derp_;
    }
    if (msg.empty() || !d) return;
    d->send(p.key, &msg[0], msg.size());
    if (count == 1 || count % 10 == 0) opt_.log("tailnet: CallMeMaybe a " + p.host + " (" + addr + ", n. " + std::to_string(count) + ")");
    std::vector<std::string> t = pingTargets(k, p);
    for (size_t i = 0; i < t.size(); i++) sendPing(p, t[i]);
    {
        std::lock_guard<std::mutex> l(udpMu_);
        pingSent_[k] = nowMs();
    }
}

void Tailnet::onDiscoDerp(const uint8_t src[32], const uint8_t* p, size_t n) {
    if (n <= 38) return;
    const uint8_t* sender = p + 6;
    Peer peer;
    bool found = false;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end(); ++it)
            if (memcmp(it->second.key, src, 32) == 0 && it->second.hasDisco && memcmp(it->second.disco, sender, 32) == 0) {
                peer = it->second;
                found = true;
                break;
            }
    }
    if (!found) return;
    Bytes plain;
    if (!boxOpen(sender, disco_, p + 38, n - 38, plain) || plain.size() < 2 || plain[0] != 3) return;
    std::vector<std::string> eps;
    for (size_t o = 2; o + 18 <= plain.size(); o += 18) {
        static const uint8_t v4[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(&plain[o], v4, 12) != 0) continue;   // IPv6: this client is IPv4 only
        char b[32];
        snprintf(b, sizeof b, "%u.%u.%u.%u:%u", plain[o + 12], plain[o + 13], plain[o + 14], plain[o + 15],
                 (unsigned)(plain[o + 16] << 8 | plain[o + 17]));
        eps.push_back(b);
    }
    std::string k = hex(peer.key, 32), list;
    for (size_t i = 0; i < eps.size(); i++) list += (i ? " " : "") + eps[i];
    bool answer;
    {
        std::lock_guard<std::mutex> l(udpMu_);
        cmmEps_[k] = eps;
        int64_t now = nowMs();
        punchUntil_[k] = now + 10000;
        std::map<std::string, int64_t>::iterator cs = cmmSent_.find(k);
        answer = !stunAddr_.empty() && (cs == cmmSent_.end() || now - cs->second > 2000);
    }
    opt_.log("tailnet: CallMeMaybe da " + peer.host + ": " + (list.empty() ? "-" : list));
    if (answer) sendCallMeMaybe(peer);   // our address back + pings to all of its addresses
    else pingPeer(k);
}

std::shared_ptr<TcpConn> Tailnet::dial(const std::string& host, uint16_t port, int timeoutMs, std::string& err) {
    int64_t end = nowMs() + timeoutMs;
    Peer target;
    bool found = false;
    uint32_t self = 0;
    uint32_t wantIp = parseIp4(host);
    while (nowMs() < end && running_) {
        {
            std::lock_guard<std::mutex> l(mu_);
            self = selfIp_;
            for (std::map<std::string, Peer>::iterator it = peers_.begin(); it != peers_.end(); ++it)
                if ((wantIp && it->second.ip == wantIp) || (!wantIp && it->second.host == host)) {
                    target = it->second;
                    found = true;
                }
        }
        if (found && self) break;
        sleepMs(200);
    }
    if (!found || !self) {
        err = !running_ ? "tailnet spenta" : !self ? "tailnet: " + status() : host + " non è nella tailnet";
        return nullptr;
    }
    if (!target.derp && target.endpoints.empty()) {
        err = host + " è offline (nessun DERP)";
        return nullptr;
    }
    if (target.derp) {
        std::string derpErr;
        if (!ensureDerp(target.derp, derpErr) && target.endpoints.empty()) {
            err = derpErr;
            return nullptr;
        }
    }
    // Learn our UDP source to the Pixel at once (the direct path is up in a fraction of a second when it works).
    pingPeer(hex(target.key, 32));
    if (holePunch_) {
        bool known;
        {
            std::lock_guard<std::mutex> l(udpMu_);
            cmmCount_[hex(target.key, 32)] = 0;
            cmmSent_.erase(hex(target.key, 32));   // the ticker sends it as soon as STUN has answered
            known = !stunAddr_.empty();
        }
        if (known && target.derp) sendCallMeMaybe(target);
    }
    std::shared_ptr<WgPeer> wg;
    {
        std::lock_guard<std::mutex> l(mu_);
        peerFor(target.key);
        wg = wg_[hex(target.key, 32)];
    }
    std::weak_ptr<WgPeer> ww = wg;
    std::shared_ptr<TcpConn> t(new TcpConn(self, target.ip, port, [ww](const uint8_t* p, size_t n) {
        if (std::shared_ptr<WgPeer> w = ww.lock()) w->sendIp(p, n);
    }));
    {
        std::lock_guard<std::mutex> l(mu_);
        conns_.push_back(t);
    }
    int left = (int)(end - nowMs());
    if (!t->open(left > 3000 ? left : 3000, err)) return nullptr;
    return t;
}

}  // namespace ts
