// Blocking TCP (Winsock on the phone, BSD sockets on the PC) and TLS 1.2 client streams (mbedTLS).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "tscrypto.h"

namespace ts {

int64_t nowMs();
std::string resolve4(const std::string& host, int port);   // "a.b.c.d:port", "" if DNS fails (blocking)
void sleepMs(int ms);

// Test knobs for the PC bench (tools/remotonet): a slow mobile downlink is a DERP connection read slowly, so the
// relay's own buffers and queue fill as they do for the Lumia on 4G. All off (0/false) on the phone.
struct Bench {
    volatile int derpReadBps = 0;   // DERP stream read at most this many bytes per second; < 0 = not at all
    int rcvBuf = 0;        // SO_RCVBUF of new TCP sockets (the "base station" buffer in front of the reader)
    bool noUdp = false;    // no direct UDP path: everything through DERP, as from a carrier NAT
    int lossPerMille = 0;  // received DERP packets thrown away (a lossy path, for the TCP's recovery)
    bool noLan = false;    // UDP to/from private addresses dropped (hole punching test: only the public path)
};
extern Bench bench;

struct Stream {
    virtual ~Stream() {}
    // reads up to n bytes; 0 = closed/error, -1 = timeout
    virtual int read(uint8_t* p, size_t n, int timeoutMs) = 0;
    virtual bool write(const uint8_t* p, size_t n) = 0;
    virtual void close() = 0;   // may be called from another thread to unblock a read
    bool readFull(uint8_t* p, size_t n, int timeoutMs);
    bool write(const std::string& s) { return write((const uint8_t*)s.data(), s.size()); }
};

class TcpStream : public Stream {
public:
    static std::unique_ptr<TcpStream> connect(const std::string& host, int port, int timeoutMs, std::string& err);
    ~TcpStream();
    int read(uint8_t* p, size_t n, int timeoutMs) override;
    bool write(const uint8_t* p, size_t n) override;
    using Stream::write;
    void close() override;
    bool waitReadable(int timeoutMs);   // false on timeout
    bool waitWritable(int timeoutMs);   // false on timeout
    int trySend(const uint8_t* p, size_t n);   // one non-blocking send: bytes taken, 0 = would block, -1 = error
private:
    TcpStream() : fd_(-1), closed_(false) {}
    long long fd_;
    volatile bool closed_;
};

// TLS over a TcpStream. The certificate is not checked: everything sent through it (DERP) is end-to-end
// encrypted and authenticated by WireGuard and NaCl box, so the TLS layer only has to get through networks.
class TlsStream : public Stream {
public:
    static std::unique_ptr<TlsStream> connect(const std::string& host, int port, int timeoutMs, std::string& err);
    ~TlsStream();
    int read(uint8_t* p, size_t n, int timeoutMs) override;
    bool write(const uint8_t* p, size_t n) override;
    using Stream::write;
    void close() override;
    struct Impl;
private:
    TlsStream();
    std::unique_ptr<Impl> impl_;
};

// Reads an HTTP/1.1 response head up to and including CRLFCRLF (byte by byte: nothing after it is consumed).
bool readHttpHead(Stream& s, std::string& head, int timeoutMs);

// A UDP socket: the direct path to a tailnet peer (disco + WireGuard), next to DERP. One reader thread;
// "from" is "a.b.c.d:port". IPv4 only, like the rest of this client.
class Udp {
public:
    typedef std::function<void(const uint8_t* p, size_t n, const std::string& from)> RecvFn;
    explicit Udp(RecvFn onRecv);
    ~Udp();
    bool open(std::string& err);   // bind 0.0.0.0:0 and start reading
    void close();
    bool sendTo(const std::string& addr, const uint8_t* p, size_t n);
    std::string local() const { return local_; }

private:
    void loop();
    RecvFn onRecv_;
    std::atomic<long long> fd_;
    std::atomic<bool> closed_;
    std::thread thread_;
    std::string local_;
};

}  // namespace ts
