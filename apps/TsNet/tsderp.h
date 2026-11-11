// DERP client: Tailscale's relay over TLS (port 443). Packets addressed by WireGuard node key.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "tscontrol.h"

namespace ts {

class Derp {
public:
    typedef std::function<void(const uint8_t src[32], const uint8_t* p, size_t n)> RecvFn;
    Derp(LogFn log, RecvFn onRecv) : log_(log), onRecv_(onRecv) {}
    ~Derp();
    bool connect(const std::string& host, const uint8_t nodePriv[32], std::string& err);
    // Queued for the writer thread, never blocks: the caller is often the reader thread itself (TCP acks are sent
    // while a packet is handled). False = the queue is full and the oldest packet was dropped for this one.
    bool send(const uint8_t dst[32], const uint8_t* p, size_t n);
    void close();
    bool alive() const { return alive_; }
    const std::string& host() const { return host_; }

private:
    bool writeFrame(uint8_t type, const uint8_t* a, size_t an, const uint8_t* b = nullptr, size_t bn = 0);
    bool queueFrame(uint8_t type, const uint8_t* a, size_t an, const uint8_t* b = nullptr, size_t bn = 0);
    void readerLoop();
    void writerLoop();
    LogFn log_;
    RecvFn onRecv_;
    std::unique_ptr<TlsStream> tls_;
    std::string host_;
    volatile bool alive_ = false;
    std::mutex closeMu_;
    std::thread reader_, writer_;
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<std::string> queue_;   // whole frames
    size_t queued_ = 0;               // bytes in queue_
};

}  // namespace ts
