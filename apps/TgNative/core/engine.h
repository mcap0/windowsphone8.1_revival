#ifndef TG_CORE_ENGINE_H
#define TG_CORE_ENGINE_H

// Asynchronous MTProto 2.0 engine, portable C++11. The platform owns the sockets and the clock and calls the
// engine from ONE thread (or under one lock): onConnected / onData / onClosed / tick. The engine keeps one TCP
// connection (intermediate transport) per data center, creates auth keys, wraps the first call of a connection
// in invokeWithLayer(initConnection), handles acks, salts, time sync, gzip, migrations (PHONE/USER/NETWORK/
// FILE_MIGRATE), FLOOD_WAIT, authorization export/import for other DCs, pings and reconnects.

#include "tlobj.h"

#include <functional>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

namespace tg {

struct RpcErr {
    int code;            // 0 = ok; MTProto error code; -1 = local (network, parse)
    std::string message; // e.g. "FLOOD_WAIT_30", "SESSION_PASSWORD_NEEDED"
    RpcErr() : code(0) {}
    RpcErr(int c, const std::string& m) : code(c), message(m) {}
    bool ok() const { return code == 0; }
};

typedef std::function<void(const TlValue& result, const RpcErr& err)> RpcCallback;

struct EngineConfig {
    int apiId;
    std::string deviceModel, systemVersion, appVersion, langCode;
    bool testMode;
    EngineConfig() : apiId(0), langCode("it"), testMode(false) {}
};

class IEngineHost {
public:
    virtual ~IEngineHost() {}
    virtual void netConnect(int conn, const std::string& host, int port) = 0;
    virtual void netSend(int conn, const std::vector<uint8_t>& data) = 0;
    virtual void netClose(int conn) = 0;
    virtual void randomBytes(uint8_t* out, std::size_t n) = 0;
    virtual int64_t unixTimeMs() = 0;
    virtual void onUpdates(const TlValue& updates) {}
    virtual void onSessionReset() {}          // new_session_created on the home DC: updates may be missing
    virtual void onStateChanged() {}          // keys/salts/home DC changed: persist Engine::save()
    virtual void onAuthLost() {}              // AUTH_KEY_UNREGISTERED on the home DC while authorized
    virtual void onOnline(bool online) {}     // home connection ready / lost
    virtual void log(const std::string& line) {}
};

class Engine {
public:
    Engine(IEngineHost* host, const EngineConfig& cfg);

    std::string save() const;
    bool load(const std::string& blob);

    int homeDc() const { return homeDc_; }
    bool authorized() const { return authorized_; }
    void setAuthorized(bool v);                  // after auth.signIn / checkPassword succeeded
    void setDcAddress(int dc, const std::string& ip, int port);
    void logOutLocal();                          // forget all keys

    // Sends fn (a function object). dc 0 = home DC. Callbacks run inside onData/tick.
    uint64_t invoke(const TlValue& fn, const RpcCallback& cb, int dc = 0);
    void cancel(uint64_t reqId);

    void start();      // connect to the home DC and keep it alive
    void stop();       // close everything (app suspended); pending requests are kept for start()

    void onConnected(int conn);
    void onData(int conn, const uint8_t* data, std::size_t size);
    void onClosed(int conn);
    void tick();       // call about twice a second

    bool online() const;
    std::size_t pendingCount() const { return reqs_.size(); }   // requests not answered yet

private:
    struct DcKey {
        bool has;
        uint8_t key[256];
        uint64_t keyId;
        uint64_t salt;
        bool imported;
        DcKey();
    };
    struct Req {
        uint64_t id;
        TlValue fn;
        RpcCallback cb;
        int dc;
        uint64_t msgId;
        int64_t sentMs;
        int64_t notBeforeMs;
        int tries;
        bool isImport;
        Req() : id(0), dc(0), msgId(0), sentMs(0), notBeforeMs(0), tries(0), isImport(false) {}
    };
    struct Conn {
        enum St { Idle, Connecting, Handshake, Ready };
        int dc;
        St st;
        std::vector<uint8_t> rx;
        int hsStep;
        uint8_t nonce[16], serverNonce[16], newNonce[32];
        uint64_t sessionId;
        uint64_t lastMsgId;
        uint32_t seq;
        bool initSent;
        bool importing;
        std::vector<uint64_t> acks;
        int64_t stateSinceMs, lastRxMs, lastPingMs, retryAtMs;
        int failures;
        Conn();
    };

    Conn& conn(int dc);
    DcKey& key(int dc) { return keys_[dc]; }
    int64_t now() { return host_->unixTimeMs(); }
    void connectDc(int dc);
    void closeConn(Conn& c, const char* why);
    void pump(Conn& c);
    void sendPlain(Conn& c, const std::vector<uint8_t>& body);
    uint64_t sendEncrypted(Conn& c, const std::vector<uint8_t>& body, bool contentRelated);
    uint64_t nextMsgId(Conn& c);
    void flushAcks(Conn& c);
    void handshakeStart(Conn& c);
    void handshakeStep(Conn& c, const std::vector<uint8_t>& body);
    void handlePacket(Conn& c, const std::vector<uint8_t>& packet);
    void handleBody(Conn& c, uint64_t msgId, uint32_t seqNo, const std::vector<uint8_t>& body);
    void handleResult(Conn& c, uint64_t reqMsgId, const std::vector<uint8_t>& raw);
    void handleError(Req& r, const RpcErr& e);
    void resendAll(int dc);
    void startImport(int dc);
    Req* findByMsg(uint64_t msgId);
    void finish(uint64_t reqId, const TlValue& v, const RpcErr& e);
    bool needsImport(int dc) const;
    void log(const std::string& s) { host_->log(s); }

    IEngineHost* host_;
    EngineConfig cfg_;
    int homeDc_;
    bool authorized_;
    bool running_;
    int64_t timeOffsetMs_;
    uint64_t nextReqId_;
    std::map<int, DcKey> keys_;
    std::map<int, Conn> conns_;
    std::map<int, std::pair<std::string, int> > addrs_;
    std::map<uint64_t, Req> reqs_;
};

} // namespace tg

#endif
