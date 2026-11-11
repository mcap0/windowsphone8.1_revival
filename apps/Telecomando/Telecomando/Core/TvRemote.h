// Samsung TV remote (Tizen, 2016+) over the TV's WebSocket API, portable C++11 on TsNet's blocking TLS.
// wss://<tv>:8002/api/v2/channels/samsung.remote.control?name=<base64>&token=<t>: the plain ws port 8001 is refused
// by the 2023 TVs ("ms.channel.unauthorized"). The first connection of a name makes the TV ask "Allow?"; its answer
// carries the token, which later connections pass so the TV no longer asks. Checked against the UE43CU7170
// (tools/tvlab/remote.py).
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ts { class TlsStream; }

namespace tv {

class Remote {
public:
    Remote();
    ~Remote();
    std::function<void(const std::string&)> log;

    // Blocking: TLS, WebSocket upgrade, then waits for the TV's answer (up to timeoutMs: the user may have to
    // accept on the TV). Returns "" when connected; token is updated when the TV sends one.
    std::string connect(const std::string& host, const std::string& name, std::string& token, int timeoutMs);
    // cmd: "Click", or "Press" / "Release" for a key held down
    bool sendKey(const std::string& key, const char* cmd = "Click");
    // Reads what the TV sends (answers pings). false when the connection is gone; true on timeout.
    bool poll(int timeoutMs);
    bool connected() const;
    void close();   // shuts the connection; safe from any thread (the stream itself is released by its users)

private:
    bool sendText(const std::string& text);
    bool sendFrame(int opcode, const std::string& payload);
    int readFrame(std::string& payload, int timeoutMs);   // opcode, 0 = closed/error, -1 = timeout
    std::shared_ptr<ts::TlsStream> current() const;
    std::shared_ptr<ts::TlsStream> stream_;   // copied under mu_ by whoever uses it, so close() never frees it under them
    mutable std::mutex mu_;
    std::mutex sendMu_;
};

// Wake-on-LAN magic packet to the TV's MAC ("00:11:22:33:44:55"), broadcast on port 9 (and to the /24 of host).
// The TV wakes from standby over Wi-Fi only with "Accendi con dispositivo mobile" on. "" or an error.
std::string wakeOnLan(const std::string& mac, const std::string& host);

std::string base64(const std::string& in);

}  // namespace tv
