#include "tstcp.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace ts {

enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };
// Receive window. Remoto bounds what it has in flight itself (frames acknowledged by the Lumia, Remoto.java), so
// the window only has to be out of its way: 256 KB is 2 Mbit/s at a 1 s round trip, 20 Mbit/s at 100 ms. (32 KB
// capped a 100 ms path at 2.6 Mbit/s and still let a slow one queue half a second.)
static const uint32_t kRcvWindow = 256u << 10;
static const int kRcvShift = 5;
static const int64_t kMinRto = 200;   // as Linux: a lost touch or ack waits 200 ms, not a second

static inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static inline bool seqLt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seqLe(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint32_t csumAdd(uint32_t sum, const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (uint32_t)(p[n - 1] << 8);
    return sum;
}
static uint16_t csumFold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

TcpConn::TcpConn(uint32_t localIp, uint32_t remoteIp, uint16_t remotePort, SendIpFn sendIp)
    : sendIp_(sendIp), lip_(localIp), rip_(remoteIp), rport_(remotePort) {
    uint8_t r[6];
    randomBytes(r, 6);
    lport_ = (uint16_t)(40000 + (r[0] << 8 | r[1]) % 20000);
    iss_ = get32(r + 2);
}

void TcpConn::sendSeg(uint32_t seq, uint8_t flags, const uint8_t* data, size_t n, bool syn) {
    sendSegOpt(seq, flags, data, n, syn, nullptr, 0);
}

void TcpConn::sendAck() {
    if (!sackOk_ || ooo_.empty()) {
        sendSeg(sndNxt_, ACK, nullptr, 0);
        return;
    }
    // Merged ranges of ooo_, the most recently changed first (RFC 2018), at most 3 (12 + 2 + 24 bytes of options).
    std::vector<std::pair<uint64_t, uint64_t> > blocks;
    for (std::map<uint64_t, std::string>::iterator it = ooo_.begin(); it != ooo_.end(); ++it) {
        uint64_t a = it->first, b = it->first + it->second.size();
        if (!blocks.empty() && a <= blocks.back().second) blocks.back().second = std::max(blocks.back().second, b);
        else blocks.push_back(std::make_pair(a, b));
    }
    for (size_t i = 0; i < blocks.size(); i++)
        if (lastOooKey_ >= blocks[i].first && lastOooKey_ < blocks[i].second && i > 0) {
            std::pair<uint64_t, uint64_t> b = blocks[i];
            blocks.erase(blocks.begin() + i);
            blocks.insert(blocks.begin(), b);
            break;
        }
    size_t nb = std::min<size_t>(blocks.size(), 3);
    uint8_t opts[40];
    opts[0] = 1; opts[1] = 1; opts[2] = 5; opts[3] = (uint8_t)(2 + 8 * nb);
    for (size_t i = 0; i < nb; i++) {
        put32(opts + 4 + 8 * i, rcvNxt_ + (uint32_t)(blocks[i].first - rcvOffset_));
        put32(opts + 8 + 8 * i, rcvNxt_ + (uint32_t)(blocks[i].second - rcvOffset_));
    }
    sendSegOpt(sndNxt_, ACK, nullptr, 0, false, opts, 4 + 8 * nb);
}

// In-order data goes to rcvBuf_ (and pulls in what was waiting behind it); data past a hole waits in ooo_.
void TcpConn::receiveData(uint32_t seq, const uint8_t* data, size_t n) {
    int32_t delta = (int32_t)(seq - rcvNxt_);
    if (delta < 0) {   // starts before rcvNxt_: a resend overlapping what we have
        if ((size_t)(-delta) >= n) return;
        data += -delta;
        n -= (size_t)(-delta);
        delta = 0;
    }
    size_t room = kRcvWindow > rcvBuf_.size() - rcvOff_ ? kRcvWindow - (rcvBuf_.size() - rcvOff_) : 0;
    if ((size_t)delta >= room) return;   // beyond the window we offered
    if ((size_t)delta + n > room) n = room - (size_t)delta;
    uint64_t key = rcvOffset_ + (uint64_t)delta;
    if (delta > 0) {
        std::map<uint64_t, std::string>::iterator it = ooo_.find(key);
        if (it == ooo_.end() || it->second.size() < n) {
            if (it != ooo_.end()) oooBytes_ -= it->second.size();
            ooo_[key].assign((const char*)data, n);
            oooBytes_ += n;
        }
        lastOooKey_ = key;
        return;
    }
    rcvBuf_.append((const char*)data, n);
    rcvNxt_ += (uint32_t)n;
    rcvOffset_ += n;
    while (!ooo_.empty()) {
        std::map<uint64_t, std::string>::iterator it = ooo_.begin();
        if (it->first > rcvOffset_) break;
        uint64_t end = it->first + it->second.size();
        if (end > rcvOffset_) {
            size_t skip = (size_t)(rcvOffset_ - it->first);
            rcvBuf_.append(it->second, skip, std::string::npos);
            rcvNxt_ += (uint32_t)(end - rcvOffset_);
            rcvOffset_ = end;
        }
        oooBytes_ -= it->second.size();
        ooo_.erase(it);
    }
    cv_.notify_all();
}

void TcpConn::sendSegOpt(uint32_t seq, uint8_t flags, const uint8_t* data, size_t n, bool syn, const uint8_t* opts,
                         size_t optBytes) {
    size_t optLen = syn ? 12 : optBytes;
    size_t tcpLen = 20 + optLen + n;
    Bytes pkt(20 + tcpLen, 0);
    uint8_t* ip = pkt.data();
    ip[0] = 0x45;
    put16(ip + 2, (uint32_t)pkt.size());
    put16(ip + 4, ipId_++);
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 6;
    put32(ip + 12, lip_);
    put32(ip + 16, rip_);
    put16(ip + 10, csumFold(csumAdd(0, ip, 20)));
    uint8_t* t = ip + 20;
    put16(t, lport_);
    put16(t + 2, rport_);
    put32(t + 4, seq);
    put32(t + 8, (flags & ACK) ? rcvNxt_ : 0);
    t[12] = (uint8_t)(((20 + optLen) / 4) << 4);
    t[13] = flags;
    size_t unread = rcvBuf_.size() - rcvOff_ + oooBytes_;
    uint32_t win = unread >= kRcvWindow ? 0 : (uint32_t)(kRcvWindow - unread);
    put16(t + 14, syn ? 65535 : std::min<uint32_t>(win >> kRcvShift, 65535));
    if (syn) {   // MSS, NOP + window scale, NOP NOP + SACK permitted
        uint8_t* o = t + 20;
        o[0] = 2; o[1] = 4; put16(o + 2, mss_);
        o[4] = 1;
        o[5] = 3; o[6] = 3; o[7] = kRcvShift;
        o[8] = 1; o[9] = 1; o[10] = 4; o[11] = 2;
    } else if (opts) {
        memcpy(t + 20, opts, optLen);
    }
    if (n) memcpy(t + 20 + optLen, data, n);
    uint8_t pseudo[12];
    put32(pseudo, lip_);
    put32(pseudo + 4, rip_);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(pseudo + 10, (uint32_t)tcpLen);
    put16(t + 16, csumFold(csumAdd(csumAdd(0, pseudo, 12), t, tcpLen)));
    sendIp_(pkt.data(), pkt.size());
}

bool TcpConn::open(int timeoutMs, std::string& err) {
    std::unique_lock<std::mutex> l(mu_);
    state_ = SYN_SENT;
    sndUna_ = iss_;
    sndNxt_ = iss_ + 1;
    sndMax_ = sndNxt_;
    rto_ = 1000;
    rtoAt_ = nowMs() + rto_;
    sendSeg(iss_, SYN, nullptr, 0, true);
    cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return state_ != SYN_SENT; });
    if (state_ == EST) return true;
    err = error_.empty() ? "connessione al ponte scaduta" : error_;
    if (state_ == SYN_SENT) state_ = DEAD;
    return false;
}

bool TcpConn::matches(const uint8_t* ip, size_t n) const {
    if (n < 40 || (ip[0] >> 4) != 4 || ip[9] != 6) return false;
    size_t ihl = (ip[0] & 15) * 4;
    if (n < ihl + 20) return false;
    return get32(ip + 12) == rip_ && get32(ip + 16) == lip_ && get16(ip + ihl) == rport_ && get16(ip + ihl + 2) == lport_;
}

void TcpConn::die(const char* why) {
    if (state_ != DEAD && error_.empty()) error_ = why;
    state_ = DEAD;
    rtoAt_ = 0;
    cv_.notify_all();
}

void TcpConn::input(const uint8_t* ip, size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    size_t ihl = (ip[0] & 15) * 4;
    size_t total = get16(ip + 2);
    if (total < n) n = total;
    const uint8_t* t = ip + ihl;
    size_t tlen = n - ihl;
    size_t doff = (t[12] >> 4) * 4;
    if (doff < 20 || doff > tlen) return;
    uint8_t flags = t[13];
    uint32_t seq = get32(t + 4), ack = get32(t + 8);
    uint32_t win = get16(t + 14);
    const uint8_t* data = t + doff;
    size_t dlen = tlen - doff;
    if (flags & RST) {
        die("connessione rifiutata dal ponte");
        return;
    }
    if (state_ == SYN_SENT) {
        if ((flags & (SYN | ACK)) != (SYN | ACK) || ack != iss_ + 1) return;
        int shift = 0;
        bool ws = false;
        for (size_t i = 20; i < doff;) {   // options: MSS, window scale
            uint8_t k = t[i];
            if (k == 0) break;
            if (k == 1) { i++; continue; }
            if (i + 1 >= doff || t[i + 1] < 2) break;
            if (k == 2 && t[i + 1] == 4) mss_ = std::min<uint32_t>(mss_, get16(t + i + 2));
            if (k == 3 && t[i + 1] == 3) { ws = true; shift = t[i + 2]; }
            if (k == 4 && t[i + 1] == 2) sackOk_ = true;
            i += t[i + 1];
        }
        sndShift_ = ws ? std::min(shift, 14) : 0;
        sndWnd_ = win;   // never scaled in a SYN
        rcvNxt_ = seq + 1;
        sndUna_ = iss_ + 1;
        state_ = EST;
        rtoAt_ = 0;
        sendSeg(sndNxt_, ACK, nullptr, 0);
        cv_.notify_all();
        return;
    }
    if (state_ != EST && state_ != FIN_WAIT) return;
    int64_t now = nowMs();
    if (flags & ACK) {
        // Valid up to the highest byte ever sent (RFC 793), not just sndNxt_: after a timeout sndNxt_ goes back to
        // sndUna_ while the peer already holds what was in flight and acks all of it at once. Dropping that ack left
        // the connection retransmitting one segment until it died (FotoPixel over DERP on mobile, 2026-11-02).
        if (seqLt(sndUna_, ack) && seqLe(ack, sndMax_)) {
            uint32_t acked = ack - sndUna_;
            size_t bytes = std::min<size_t>(acked, sndBuf_.size());
            sndBuf_.erase(0, bytes);
            if (acked > bytes && finQueued_) finSent_ = true;   // the ack covers our FIN too
            if (seqLt(sndNxt_, ack)) { sndNxt_ = ack; acksAhead_++; }   // already there: do not send it again
            sndUna_ = ack;
            dupAcks_ = 0;
            retries_ = 0;
            if (rttSeqTime_ && seqLe(rttSeq_, ack)) {
                double r = (double)(now - rttSeqTime_);
                if (srtt_ == 0) { srtt_ = r; rttvar_ = r / 2; }
                else { rttvar_ = 0.75 * rttvar_ + 0.25 * (srtt_ > r ? srtt_ - r : r - srtt_); srtt_ = 0.875 * srtt_ + 0.125 * r; }
                rto_ = std::max<int64_t>(kMinRto, std::min<int64_t>(30000, (int64_t)(srtt_ + 4 * rttvar_)));
                rttSeqTime_ = 0;
            }
            double segs = (double)bytes / mss_;
            if (cwnd_ < ssthresh_) cwnd_ += segs;
            else cwnd_ += segs / cwnd_;
            if (cwnd_ > 512) cwnd_ = 512;
            rtoAt_ = sndUna_ != sndNxt_ ? now + rto_ : 0;
            cv_.notify_all();
        } else if (ack == sndUna_ && dlen == 0 && sndUna_ != sndNxt_ && !(flags & (SYN | FIN))) {
            if (++dupAcks_ == 3) {   // fast retransmit of the first missing segment
                ssthresh_ = std::max(cwnd_ / 2, 2.0);
                cwnd_ = ssthresh_;
                size_t k = std::min<size_t>(mss_, sndBuf_.size());
                if (k) sendSeg(sndUna_, ACK | PSH, (const uint8_t*)sndBuf_.data(), k);
                rttSeqTime_ = 0;
            }
        }
        sndWnd_ = win << sndShift_;
    }
    bool needAck = false;
    if (dlen > 0) {
        if (!peerFin_) receiveData(seq, data, dlen);
        needAck = true;   // in order: ack; duplicate/out of order: a dup ack with SACK blocks, the peer resends the hole
    }
    if ((flags & FIN) && seq + (uint32_t)dlen == rcvNxt_ && !peerFin_) {
        rcvNxt_++;
        peerFin_ = true;
        needAck = true;
        cv_.notify_all();
    }
    if (needAck) sendAck();
    pump();
    if (finSent_ && peerFin_ && sndUna_ == sndNxt_) die("chiusa");
}

void TcpConn::pump() {
    if (state_ != EST && state_ != FIN_WAIT) return;
    int64_t now = nowMs();
    while (true) {
        uint32_t inflight = sndNxt_ - sndUna_;
        if (finSent_) inflight--;   // the FIN takes a sequence number but no buffer byte
        size_t unsent = sndBuf_.size() > inflight ? sndBuf_.size() - inflight : 0;
        double w = std::min<double>(sndWnd_, cwnd_ * mss_);
        if (unsent > 0 && !finSent_ && inflight < w) {
            size_t k = std::min<size_t>(std::min<size_t>(mss_, unsent), (size_t)(w - inflight));
            if (k == 0) break;
            sendSeg(sndNxt_, ACK | PSH, (const uint8_t*)sndBuf_.data() + inflight, k);
            if (!rttSeqTime_) { rttSeqTime_ = now; rttSeq_ = sndNxt_ + (uint32_t)k; }
            sndNxt_ += (uint32_t)k;
            if (seqLt(sndMax_, sndNxt_)) sndMax_ = sndNxt_;
            if (!rtoAt_) rtoAt_ = now + rto_;
            continue;
        }
        if (unsent == 0 && finQueued_ && !finSent_) {
            sendSeg(sndNxt_, FIN | ACK, nullptr, 0);
            sndNxt_++;
            if (seqLt(sndMax_, sndNxt_)) sndMax_ = sndNxt_;
            finSent_ = true;
            if (!rtoAt_) rtoAt_ = now + rto_;
        }
        break;
    }
}

void TcpConn::tick() {
    std::lock_guard<std::mutex> l(mu_);
    int64_t now = nowMs();
    if (!rtoAt_ || now < rtoAt_) {
        if (state_ == EST && sndWnd_ == 0 && sndBuf_.size() > (size_t)(sndNxt_ - sndUna_) && !rtoAt_) rtoAt_ = now + rto_;
        return;
    }
    if (++retries_ > 10) {
        die("il ponte non risponde (tailnet)");
        return;
    }
    rto_ = std::min<int64_t>(rto_ * 2, 30000);
    rtoAt_ = now + rto_;
    if (state_ == SYN_SENT) {
        sendSeg(iss_, SYN, nullptr, 0, true);
        return;
    }
    // timeout: go back to the first unacked byte with a small window
    timeouts_++;
    ssthresh_ = std::max(cwnd_ / 2, 2.0);
    cwnd_ = 1;
    sndNxt_ = sndUna_;
    finSent_ = false;
    rttSeqTime_ = 0;
    if (sndWnd_ == 0 && !sndBuf_.empty()) {   // zero-window probe
        sendSeg(sndNxt_, ACK, (const uint8_t*)sndBuf_.data(), 1);
        sndNxt_++;
        if (seqLt(sndMax_, sndNxt_)) sndMax_ = sndNxt_;
        return;
    }
    pump();
}

int TcpConn::read(uint8_t* p, size_t n, int timeoutMs) {
    std::unique_lock<std::mutex> l(mu_);
    bool got = cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] {
        return rcvOff_ < rcvBuf_.size() || peerFin_ || state_ == DEAD || state_ == CLOSED;
    });
    if (rcvOff_ < rcvBuf_.size()) {
        size_t k = std::min(n, rcvBuf_.size() - rcvOff_);
        bool wasTight = rcvBuf_.size() - rcvOff_ > kRcvWindow / 2;
        memcpy(p, rcvBuf_.data() + rcvOff_, k);
        rcvOff_ += k;
        if (rcvOff_ == rcvBuf_.size()) { rcvBuf_.clear(); rcvOff_ = 0; }
        else if (rcvOff_ > (1u << 20)) { rcvBuf_.erase(0, rcvOff_); rcvOff_ = 0; }
        if (wasTight && state_ == EST) sendSeg(sndNxt_, ACK, nullptr, 0);   // window update
        return (int)k;
    }
    return got ? 0 : -1;
}

bool TcpConn::write(const uint8_t* p, size_t n) {
    std::unique_lock<std::mutex> l(mu_);
    while (n > 0) {
        if (state_ != EST || finQueued_) return false;
        cv_.wait(l, [&] { return sndBuf_.size() < (4u << 20) || state_ != EST; });
        if (state_ != EST) return false;
        size_t k = std::min<size_t>(n, 256 * 1024);
        sndBuf_.append((const char*)p, k);
        p += k;
        n -= k;
        pump();
    }
    return true;
}

std::string TcpConn::stats() {
    std::lock_guard<std::mutex> l(mu_);
    char b[320];
    snprintf(b, sizeof b, "stato %d, confermati %u, inviati %u (max %u), in coda %u, cwnd %.1f, wnd %u, rto %d, tentativi %d, "
             "timeout %u, ack oltre %u, mss %u, sack %d, fuori ordine %u B",
             (int)state_, sndUna_ - iss_ - 1, sndNxt_ - iss_ - 1, sndMax_ - iss_ - 1, (unsigned)sndBuf_.size(), cwnd_,
             sndWnd_, (int)rto_, retries_, timeouts_, acksAhead_, mss_, sackOk_ ? 1 : 0, (unsigned)oooBytes_);
    return b;
}

std::string TcpConn::error() {
    std::lock_guard<std::mutex> l(mu_);
    return error_;
}

size_t TcpConn::queued() {
    std::lock_guard<std::mutex> l(mu_);
    return rcvBuf_.size() - rcvOff_;
}

void TcpConn::close() {
    std::lock_guard<std::mutex> l(mu_);
    if (state_ == EST) {
        finQueued_ = true;
        state_ = FIN_WAIT;
        pump();
    } else if (state_ != DEAD) {
        state_ = DEAD;
    }
    cv_.notify_all();
}

}  // namespace ts
