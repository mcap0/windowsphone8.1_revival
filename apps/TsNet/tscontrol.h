// Tailscale control protocol ("ts2021"): Noise IK over an HTTP upgrade on port 80 of controlplane.tailscale.com,
// then HTTP/2 inside the Noise stream. Only what the client needs: POST requests, response bodies read as streams.
#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "tsio.h"

namespace ts {

typedef std::function<void(const std::string&)> LogFn;

class Control {
public:
    explicit Control(LogFn log) : log_(log) {}
    ~Control();
    bool connect(const uint8_t machinePriv[32], std::string& err);
    void close();
    bool alive() const { return alive_; }

    // starts a POST; returns the stream id or 0
    unsigned post(const std::string& path, const std::string& json);
    // HTTP status of a stream once its headers arrived (0 = not yet, -1 = stream reset / connection gone)
    int status(unsigned id, int timeoutMs);
    // reads body bytes: >0 count, 0 = body ended (or connection gone), -1 = timeout
    int readBody(unsigned id, uint8_t* p, size_t n, int timeoutMs);
    bool readBodyFull(unsigned id, uint8_t* p, size_t n, int timeoutMs);
    std::string readAll(unsigned id, int timeoutMs);
    void forget(unsigned id);

private:
    struct H2Stream {
        int status = 0;
        bool ended = false;
        std::string buf;
        size_t off = 0;
    };
    bool noiseWrite(const uint8_t* p, size_t n);
    bool noiseRead(uint8_t* p, size_t n);
    bool frame(uint8_t type, uint8_t flags, unsigned stream, const uint8_t* p, size_t n);
    void readerLoop();

    LogFn log_;
    std::unique_ptr<TcpStream> tcp_;
    uint8_t txKey_[32], rxKey_[32];
    uint64_t txNonce_ = 0, rxNonce_ = 0;
    std::string rxPlain_;
    size_t rxOff_ = 0;
    std::mutex wmu_, postMu_, closeMu_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<unsigned, H2Stream> streams_;
    unsigned nextId_ = 1;
    volatile bool alive_ = false;
    std::thread reader_;
};

}  // namespace ts
