// Minimal TCP client over IPv4 packets (inside WireGuard): one connection, out-of-order receive with SACK (RFC 2018),
// sliding window send with retransmission (RTO, fast retransmit), window scaling. Enough for the framed protocols of
// the bridge and Remoto.
#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "tsio.h"

namespace ts {

class TcpConn : public Stream {
public:
    typedef std::function<void(const uint8_t* p, size_t n)> SendIpFn;
    TcpConn(uint32_t localIp, uint32_t remoteIp, uint16_t remotePort, SendIpFn sendIp);
    bool open(int timeoutMs, std::string& err);   // three-way handshake
    void input(const uint8_t* ip, size_t n);      // an IPv4 packet for us
    void tick();                                   // every ~50-100 ms: retransmissions
    bool matches(const uint8_t* ip, size_t n) const;

    int read(uint8_t* p, size_t n, int timeoutMs) override;
    bool write(const uint8_t* p, size_t n) override;
    using Stream::write;
    void close() override;
    bool isOpen() const { return state_ == EST; }
    std::string stats();   // for logs: bytes sent/acked, window, timers
    std::string error();   // why the connection died ("", "chiusa", "il ponte non risponde (tailnet)", ...)
    size_t queued();       // bytes received but not read yet: the catch-up's "head of stream" test

private:
    enum State { CLOSED, SYN_SENT, EST, FIN_WAIT, DEAD };
    void sendSeg(uint32_t seq, uint8_t flags, const uint8_t* data, size_t n, bool syn = false);
    void sendSegOpt(uint32_t seq, uint8_t flags, const uint8_t* data, size_t n, bool syn, const uint8_t* opts, size_t optBytes);
    void sendAck();   // with SACK blocks for what is held out of order
    void receiveData(uint32_t seq, const uint8_t* data, size_t n);
    void pump();   // sends what the windows allow
    void die(const char* why);

    std::mutex mu_;
    std::condition_variable cv_;
    SendIpFn sendIp_;
    uint32_t lip_, rip_;
    uint16_t lport_, rport_;
    State state_ = CLOSED;
    uint32_t iss_ = 0, sndUna_ = 0, sndNxt_ = 0, rcvNxt_ = 0;
    uint32_t sndMax_ = 0;     // highest sequence ever sent: after a go-back on timeout, acks up to here are still valid
    unsigned acksAhead_ = 0;  // acks accepted beyond sndNxt_ (they used to be dropped, stalling the connection)
    unsigned timeouts_ = 0;
    std::string sndBuf_;   // bytes from sndUna_ on (sent but unacked, then unsent)
    std::string rcvBuf_;
    size_t rcvOff_ = 0;
    // Out of order: segments past a hole, by stream offset (64-bit: no wrap in long sessions). Kept instead of
    // dropped: one lost packet used to cost the whole window behind it, resent one round trip at a time (seconds of
    // frozen video through the relay). The sender learns what we hold from the SACK blocks and fills just the hole.
    std::map<uint64_t, std::string> ooo_;
    size_t oooBytes_ = 0;
    uint64_t rcvOffset_ = 0;      // stream offset of rcvNxt_
    uint64_t lastOooKey_ = 0;     // the block that changed last goes first in the SACK option
    bool sackOk_ = false;
    uint32_t sndWnd_ = 65535;
    int sndShift_ = 0;
    bool peerFin_ = false, finQueued_ = false, finSent_ = false;
    uint32_t mss_ = 1200;
    double cwnd_ = 10, ssthresh_ = 64;
    int dupAcks_ = 0;
    int64_t rto_ = 1000, rtoAt_ = 0;
    int64_t rttSeqTime_ = 0;
    uint32_t rttSeq_ = 0;
    double srtt_ = 0, rttvar_ = 0;
    int retries_ = 0;
    std::string error_;
    uint16_t ipId_ = 1;
};

}  // namespace ts
