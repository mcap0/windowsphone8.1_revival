// One WireGuard peer (Noise_IKpsk2, no psk), packets carried by DERP. Initiator and responder handshakes,
// transport with replay window, rekey every 2 minutes while in use, keepalives.
#pragma once
#include <deque>
#include <functional>
#include <mutex>

#include "tscrypto.h"

namespace ts {

class WgPeer {
public:
    typedef std::function<void(const uint8_t* p, size_t n)> SendFn;   // raw WireGuard message to the peer
    typedef std::function<void(const uint8_t* p, size_t n)> IpFn;     // decrypted IP packet from the peer
    WgPeer(const uint8_t priv[32], const uint8_t peerPub[32], SendFn send, IpFn onIp);
    bool receive(const uint8_t* p, size_t n);   // true if the packet was for this peer
    void sendIp(const uint8_t* p, size_t n);
    void tick();   // call every ~100 ms
    bool up();     // a usable session exists

private:
    struct Session {
        bool valid = false, confirmed = false;   // responder sessions are confirmed by the first data packet
        uint32_t localIdx = 0, remoteIdx = 0;
        uint8_t sendKey[32], recvKey[32];
        uint64_t sendCtr = 0, recvMax = 0;
        uint64_t window[16];   // replay bitmap of the last 1024 counters
        int64_t born = 0;
        bool initiator = false;
    };
    void startHandshake();
    bool handleInitiation(const uint8_t* p, size_t n);
    bool handleResponse(const uint8_t* p, size_t n);
    bool handleData(const uint8_t* p, size_t n);
    void encryptAndSend(Session& s, const uint8_t* p, size_t n);
    bool replayOk(Session& s, uint64_t ctr);
    void mac1(uint8_t* msg, size_t macOff);
    void flushQueue();

    std::recursive_mutex mu_;
    uint8_t priv_[32], pub_[32], peer_[32];
    SendFn send_;
    IpFn onIp_;
    Session cur_, prev_, next_;   // next_ = responder session waiting for confirmation
    // in-flight initiation
    bool hsPending_ = false;
    int64_t hsSent_ = 0;
    uint32_t hsIdx_ = 0;
    uint8_t hsEPriv_[32], hsCk_[32], hsH_[32];
    int64_t lastSend_ = 0, lastRecv_ = 0, lastHsAttempt_ = 0, lastTs_ = 0;
    std::deque<Bytes> queue_;
    std::vector<Bytes> deliver_;
};

}  // namespace ts
