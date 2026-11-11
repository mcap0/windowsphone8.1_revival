#ifndef PERLECAST_SSDP_H
#define PERLECAST_SSDP_H

#include <string>

namespace PerleCast {

struct SsdpResponse {
    std::string location;
    std::string searchTarget;
    std::string usn;
    std::string server;
};

class Ssdp {
public:
    static const char* MediaRenderer;
    static const char* AvTransport;
    static std::string BuildSearchRequest(const std::string& target, int mxSeconds = 3);
    static bool ParseResponse(const std::string& message, SsdpResponse& response);
};

} // namespace PerleCast

#endif
