// Mini Tailscale node for the Lumia: registers with Tailscale's control server (auth key or login link),
// follows the netmap, reaches peers through DERP + WireGuard and opens TCP connections to them.
// DERP always, plus a direct UDP path when the peer's advertised endpoints answer disco pings (LAN, open NATs).
// No DNS, no subnet routes: just "TCP to a tailnet peer by host name".
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tscontrol.h"
#include "tsjson.h"
#include "tsderp.h"
#include "tstcp.h"
#include "tswg.h"

namespace ts {

class Tailnet {
public:
    struct Options {
        std::string statePath;    // keys and registration (created on first start)
        std::string authKey;      // tskey-auth-...; empty = interactive login link
        std::string hostname;     // name of this node in the tailnet
        LogFn log;
        LogFn onAuthUrl;          // login link to open when there is no auth key
        bool holePunch = false;   // STUN + CallMeMaybe: direct UDP through both NATs (4G <-> home) without a port forward
    };
    explicit Tailnet(const Options& o);
    ~Tailnet();
    void start();
    void stop();
    void kick();   // drop control, DERP and WireGuard sessions and their TCP connections (after a suspend): they reconnect at once
    std::string status();
    bool ready();
    // Hole punching on/off at any time (Remoto's menu). Off = exactly the old behaviour: no STUN, no endpoints of
    // ours in the netmap, CallMeMaybe ignored; the direct path then only works where the peer's endpoints answer.
    void setHolePunch(bool on);
    bool holePunch() const { return holePunch_; }
    std::string pathTo(const std::string& host);
    std::string natKind();   // "" until measured, "NAT stessa porta" or "NAT porta per destinazione" (no punching)   // "diretto a.b.c.d:port" or "relay" (for the stats line)
    // TCP connection to a peer (host name as in the admin console, or a 100.x address). Blocking.
    std::shared_ptr<TcpConn> dial(const std::string& host, uint16_t port, int timeoutMs, std::string& err);

private:
    struct Peer {
        std::string id, host;
        uint8_t key[32];
        uint8_t disco[32];   // its disco public key (for the direct UDP path)
        bool hasDisco = false;
        std::vector<std::string> endpoints;   // "ip:port" as advertised in the netmap
        uint32_t ip = 0;
        int derp = 0;
    };
    void run();
    void ticker();
    bool loadState();
    void saveState();
    std::string mapLines();   // the part of the netmap kept in the state file, so a cold start dials before control answers
    void saveMap();
    bool registerNode(Control& c);
    std::string hostinfo();
    std::string mapRequest(bool stream);
    void applyMap(const Json& m);
    bool ensureDerp(int region, std::string& err);
    void announceDerp(int region);
    void onDerp(const uint8_t src[32], const uint8_t* p, size_t n);
    WgPeer* peerFor(const uint8_t key[32]);
    void setStatus(const std::string& s);

    // Direct UDP path (disco pings + WireGuard over UDP), falling back to DERP.
    bool openUdp();
    void closeUdp();
    void onUdp(const uint8_t* p, size_t n, const std::string& from);
    void sendPing(const Peer& p, const std::string& endpoint);
    void pingPeer(const std::string& keyHex);
    void discoTick();
    bool sendDirect(const std::string& keyHex, const uint8_t* p, size_t n);
    void markDirect(const std::string& keyHex, const std::string& from);
    std::vector<std::string> pingTargets(const std::string& keyHex, const Peer& p);

    // Hole punching (Options::holePunch): STUN tells us our public UDP address, control and a CallMeMaybe over DERP
    // tell it to the peer, both sides ping each other at the same moment and the NATs let the replies through.
    void stunTick();
    bool onStun(const uint8_t* p, size_t n);
    std::string endpointsJson();   // "Endpoints":[...] for the map requests, or "" (field left out)
    void announceEndpoints();
    void sendCallMeMaybe(const Peer& p);
    void onDiscoDerp(const uint8_t src[32], const uint8_t* p, size_t n);
    Bytes discoPacket(const uint8_t peerDisco[32], const std::vector<uint8_t>& inner);

    Options opt_;
    std::atomic<bool> running_;
    std::thread runThread_, tickThread_;
    std::mutex mu_;   // netmap, status, peers, conns
    std::mutex saveMu_;
    std::string savedMap_;
    std::string status_;
    uint8_t machine_[32], node_[32], nodePub_[32], disco_[32], discoPub_[32];
    bool registered_ = false;
    uint32_t selfIp_ = 0;
    std::map<std::string, Peer> peers_;
    std::map<int, std::vector<std::string> > derpHosts_;
    int preferredDerp_ = 0;
    volatile bool derpAnnounced_ = false;   // home DERP told to this control session
    std::shared_ptr<Control> ctrl_;
    std::mutex derpMu_;
    std::shared_ptr<Derp> derp_;
    int derpRegion_ = 0;
    std::map<std::string, std::shared_ptr<WgPeer> > wg_;   // by node key hex
    std::vector<std::weak_ptr<TcpConn> > conns_;

    std::mutex udpMu_;
    std::shared_ptr<Udp> udp_;
    std::map<std::string, std::string> addrKey_;    // "ip:port" -> node key hex (peer seen at this address)
    std::map<std::string, std::string> direct_;     // node key hex -> the UDP address that works
    std::map<std::string, int64_t> udpSeen_;        // node key hex -> last UDP packet from it
    std::map<std::string, int64_t> pingSent_;       // node key hex -> last disco ping sent
    int64_t udpSince_ = 0;                          // when the UDP socket was opened (probing pace)

    std::atomic<bool> holePunch_;
    std::map<int, std::vector<std::string> > stunHosts_;   // DERP region -> "ip:port" of its STUN servers
    std::vector<std::string> stunPublic_;          // Google's and Cloudflare's (resolved by run(); first choice)
    std::string stunAddr_;                         // our public UDP address as STUN sees it ("" = not known)
    uint8_t stunTx_[12], stunTx2_[12];   // the second goes to another server: same mapped port = NAT kind check
    std::string stunAddr2_, natKind_;
    int64_t stunSent_ = 0, stunGot_ = 0;
    size_t stunNext_ = 0;
    std::string epSent_;                           // endpoints last told to control ("" = none)
    std::atomic<bool> epBusy_;
    std::thread epThread_;
    std::map<std::string, std::vector<std::string> > cmmEps_;   // node key hex -> endpoints from its CallMeMaybe
    std::map<std::string, int64_t> cmmSent_;       // node key hex -> last CallMeMaybe sent
    std::map<std::string, int> cmmCount_;          // node key hex -> CallMeMaybe sent since the last dial
    int64_t epTried_ = 0;
    std::map<std::string, int64_t> punchUntil_;    // node key hex -> fast pings until then (both sides punching)
};

}  // namespace ts
