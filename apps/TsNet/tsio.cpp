#include "tsio.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "tsmbed.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

namespace ts {

Bench bench;

static bool wouldBlock() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool Stream::readFull(uint8_t* p, size_t n, int timeoutMs) {
    int64_t end = nowMs() + timeoutMs;
    while (n > 0) {
        int left = (int)(end - nowMs());
        if (left <= 0) return false;
        int r = read(p, n, left);
        if (r == 0) return false;
        if (r < 0) continue;
        p += r;
        n -= r;
    }
    return true;
}

static void netInit() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
#endif
}

static void setBlocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long nb = on ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
#endif
}

// select() on one socket: >0 ready, 0 timeout, <0 error
static int waitSock(sock_t s, bool forWrite, int timeoutMs) {
    fd_set fs, es;
    FD_ZERO(&fs);
    FD_ZERO(&es);
    FD_SET(s, &fs);
    FD_SET(s, &es);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select((int)s + 1, forWrite ? nullptr : &fs, forWrite ? &fs : nullptr, &es, &tv);
    if (r > 0 && FD_ISSET(s, &es) && !FD_ISSET(s, &fs)) return -1;
    return r;
}

std::string resolve4(const std::string& host, int port) {
    netInit();
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return std::string();
    const uint8_t* ip = (const uint8_t*)&((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    char b[32];
    snprintf(b, sizeof b, "%u.%u.%u.%u:%d", ip[0], ip[1], ip[2], ip[3], port);
    freeaddrinfo(res);
    return b;
}

std::unique_ptr<TcpStream> TcpStream::connect(const std::string& host, int port, int timeoutMs, std::string& err) {
    netInit();
    addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;   // the phone's networks are IPv4; keeps getaddrinfo answers simple
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    int gr = getaddrinfo(host.c_str(), ps, &hints, &res);
    if (gr != 0 || !res) {
        err = "DNS " + host + " fallito";
        return nullptr;
    }
    sock_t s = INVALID_SOCKET;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (bench.rcvBuf > 0) setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bench.rcvBuf, sizeof bench.rcvBuf);
        setBlocking(s, false);
        ::connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (waitSock(s, true, timeoutMs) > 0) {
            int e = 0;
            socklen_t el = sizeof e;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &el);
            if (e == 0) break;
        }
        CLOSESOCK(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) {
        err = "connessione a " + host + " fallita";
        return nullptr;
    }
    // Non-blocking from here on: reads wait in select(), writes too (write()) or not at all (trySend(), the TLS
    // layer's way to never hold its lock while the network is busy).
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    std::unique_ptr<TcpStream> t(new TcpStream());
    t->fd_ = (long long)s;
    return t;
}

TcpStream::~TcpStream() {
    close();
    if (fd_ != -1) CLOSESOCK((sock_t)fd_);
}

bool TcpStream::waitReadable(int timeoutMs) {
    if (closed_) return true;   // let the read report the close
    return waitSock((sock_t)fd_, false, timeoutMs) != 0;
}

bool TcpStream::waitWritable(int timeoutMs) {
    if (closed_) return true;
    return waitSock((sock_t)fd_, true, timeoutMs) != 0;
}

int TcpStream::read(uint8_t* p, size_t n, int timeoutMs) {
    if (closed_) return 0;
    int w = waitSock((sock_t)fd_, false, timeoutMs);
    if (closed_ || w < 0) return 0;
    if (w == 0) return -1;
    int r = (int)recv((sock_t)fd_, (char*)p, (int)n, 0);
    if (r > 0) return r;
    if (r < 0 && wouldBlock()) return -1;   // select said readable but nothing came: a timeout to the caller
    return 0;
}

int TcpStream::trySend(const uint8_t* p, size_t n) {
    if (closed_) return -1;
#ifdef _WIN32
    int r = send((sock_t)fd_, (const char*)p, (int)n, 0);
#else
    int r = (int)send((sock_t)fd_, (const char*)p, n, MSG_NOSIGNAL);
#endif
    if (r > 0) return r;
    if (r < 0 && wouldBlock()) return 0;
    return -1;
}

bool TcpStream::write(const uint8_t* p, size_t n) {
    while (n > 0) {
        int r = trySend(p, n);
        if (r < 0) return false;
        if (r == 0) {
            if (closed_) return false;
            waitSock((sock_t)fd_, true, 1000);
            continue;
        }
        p += r;
        n -= r;
    }
    return true;
}

void TcpStream::close() {
    if (closed_) return;
    closed_ = true;
#ifdef _WIN32
    shutdown((sock_t)fd_, SD_BOTH);
#else
    shutdown((sock_t)fd_, SHUT_RDWR);
#endif
}

// ---- TLS

struct TlsStream::Impl {
    std::unique_ptr<TcpStream> tcp;
    tsm_tls* tls = nullptr;
    std::mutex mu;        // mbedTLS contexts are not thread-safe: reads and writes come from different threads
    std::mutex writeMu;   // one write at a time from start to end: a record left half-sent (WANT_WRITE) is finished first
    bool blockingRecv = true;
    int handshakeTimeout = 10000;
};

// Never blocks: a full socket buffer is WANT_WRITE, and TlsStream::write waits for room without the lock, so the
// reader thread keeps reading (it used to stop while a write waited on a slow uplink, and the relay's queue for
// us overflowed meanwhile).
static int bioSend(void* ctx, const unsigned char* p, size_t n) {
    TlsStream::Impl* im = (TlsStream::Impl*)ctx;
    int r = im->tcp->trySend(p, n);
    if (r < 0) return TSM_INTERNAL;
    return r == 0 ? TSM_WANT_WRITE : r;
}

static int bioRecv(void* ctx, unsigned char* p, size_t n) {
    TlsStream::Impl* im = (TlsStream::Impl*)ctx;
    int r = im->tcp->read(p, n, im->blockingRecv ? im->handshakeTimeout : 0);
    if (r < 0) return TSM_WANT_READ;
    return r == 0 ? TSM_EOF : r;
}

std::unique_ptr<TlsStream> TlsStream::connect(const std::string& host, int port, int timeoutMs, std::string& err) {
    std::unique_ptr<TcpStream> tcp = TcpStream::connect(host, port, timeoutMs, err);
    if (!tcp) return nullptr;
    std::unique_ptr<TlsStream> t(new TlsStream());
    t->impl_.reset(new Impl());
    Impl* im = t->impl_.get();
    im->tcp = std::move(tcp);
    im->handshakeTimeout = timeoutMs;
    im->tls = tsm_tls_new(host.c_str(), im, bioSend, bioRecv, randomBytes);   // TLS 1.2 (1.3 needs PSA entropy)
    if (!im->tls) {
        err = "TLS: init";
        return nullptr;
    }
    int r;
    while ((r = tsm_tls_handshake(im->tls)) != 0) {
        if (r == TSM_FAULT) {
            unsigned long code;
            const void* at;
            const void* ref;
            tsm_last_fault(&code, &at, &ref);
            char b[160];
            snprintf(b, sizeof b, "TLS con %s: GUASTO 0x%08lX a %p (handshake %p, delta %+ld)", host.c_str(), code, at, ref,
                     (long)((const char*)at - (const char*)ref));
            err = b;
            return nullptr;
        }
        if (r != TSM_WANT_READ && r != TSM_WANT_WRITE) {
            char b[64];
            snprintf(b, sizeof b, "TLS con %s: errore -0x%04x", host.c_str(), -r);
            err = b;
            return nullptr;
        }
        if (r == TSM_WANT_WRITE) im->tcp->waitWritable(1000);
    }
    im->blockingRecv = false;
    return t;
}

TlsStream::TlsStream() {}

TlsStream::~TlsStream() {
    if (!impl_) return;
    impl_->tcp->close();
    std::lock_guard<std::mutex> l(impl_->mu);
    tsm_tls_free(impl_->tls);
}

int TlsStream::read(uint8_t* p, size_t n, int timeoutMs) {
    int64_t end = nowMs() + timeoutMs;
    while (true) {
        {
            std::lock_guard<std::mutex> l(impl_->mu);
            int r = tsm_tls_read(impl_->tls, p, n);
            if (r > 0) return r;
            if (r != TSM_WANT_READ && r != TSM_WANT_WRITE &&
                r != TSM_NEW_TICKET)
                return 0;
        }
        int left = (int)(end - nowMs());
        if (left <= 0) return -1;
        if (!impl_->tcp->waitReadable(left)) return -1;   // waits without the lock, so writers are not blocked
    }
}

bool TlsStream::write(const uint8_t* p, size_t n) {
    // mbedTLS: after WANT_WRITE the same call is repeated until it completes. Between tries the lock is released,
    // so reads go on while the uplink is full.
    std::lock_guard<std::mutex> w(impl_->writeMu);
    std::unique_lock<std::mutex> l(impl_->mu);
    while (n > 0) {
        int r = tsm_tls_write(impl_->tls, p, n);
        if (r == TSM_WANT_READ || r == TSM_WANT_WRITE) {
            l.unlock();
            if (r == TSM_WANT_WRITE) impl_->tcp->waitWritable(200);
            else sleepMs(1);
            l.lock();
            continue;
        }
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

void TlsStream::close() { impl_->tcp->close(); }

bool readHttpHead(Stream& s, std::string& head, int timeoutMs) {
    head.clear();
    int64_t end = nowMs() + timeoutMs;
    while (head.size() < 16384) {
        uint8_t c;
        int left = (int)(end - nowMs());
        if (left <= 0 || !s.readFull(&c, 1, left)) return false;
        head += (char)c;
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) return true;
    }
    return false;
}

// ---- UDP

static bool parseIpPort(const std::string& s, uint8_t ip[4], uint16_t& port) {
    unsigned a, b, c, d, p;
    if (sscanf(s.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) != 5) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255 || p > 65535) return false;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b; ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    port = (uint16_t)p;
    return true;
}

static std::string ipPortStr(const sockaddr_in& a) {
    const uint8_t* ip = (const uint8_t*)&a.sin_addr.s_addr;   // network order: the bytes are a.b.c.d
    char b[32];
    snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(a.sin_port));
    return b;
}

static bool isPrivate(const uint8_t ip[4]) {
    return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 192 && ip[1] == 168) ||
           (ip[0] == 100 && (ip[1] & 0xc0) == 64) || ip[0] == 127;
}

Udp::Udp(RecvFn onRecv) : onRecv_(onRecv), fd_(-1), closed_(false) {}

Udp::~Udp() { close(); }

bool Udp::open(std::string& err) {
    netInit();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { err = "UDP: socket non creato"; return false; }
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0;
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) { CLOSESOCK(s); err = "UDP: bind fallito"; return false; }
    int sz = 4 << 20;   // video bursts: the kernel should not drop them while the reader is busy
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof sz);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof sz);
    socklen_t al = sizeof a;
    getsockname(s, (sockaddr*)&a, &al);
    local_ = ipPortStr(a);
    fd_ = (long long)s;
    closed_ = false;
    thread_ = std::thread(&Udp::loop, this);
    return true;
}

void Udp::loop() {
    std::vector<uint8_t> buf(65536);
    while (!closed_) {
        sock_t s = (sock_t)fd_.load();
        if (s == INVALID_SOCKET) break;
        if (waitSock(s, false, 200) <= 0) continue;
        sockaddr_in from;
        memset(&from, 0, sizeof from);
        socklen_t fl = sizeof from;
        int n = (int)recvfrom(s, (char*)&buf[0], (int)buf.size(), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        if (bench.noLan && isPrivate((const uint8_t*)&from.sin_addr.s_addr)) continue;
        if (onRecv_) onRecv_(&buf[0], (size_t)n, ipPortStr(from));
    }
}

void Udp::close() {
    if (closed_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();   // the loop wakes up from select within 200 ms
    long long f = fd_.exchange(-1);
    if (f != -1) CLOSESOCK((sock_t)f);
}

bool Udp::sendTo(const std::string& addr, const uint8_t* p, size_t n) {
    sock_t s = (sock_t)fd_.load();
    if (s == INVALID_SOCKET) return false;
    uint8_t ip[4];
    uint16_t port;
    if (!parseIpPort(addr, ip, port)) return false;
    if (bench.noLan && isPrivate(ip)) return true;
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr.s_addr, ip, 4);
    a.sin_port = htons(port);
    int r = (int)sendto(s, (const char*)p, (int)n, 0, (sockaddr*)&a, sizeof a);
    return r == (int)n;
}

}  // namespace ts
