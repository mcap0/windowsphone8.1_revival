#include "Test.h"
#include "../core/ProxyHttp.h"

int RunProxyHttpTests()
{
    using namespace PerleCast;
    HttpRequest request;
    Check(ProxyHttp::ParseRequest("GET /v/a.ts?t=5 HTTP/1.1\r\nHost: tv\r\nRange: bytes=0-\r\n\r\n", request), "parses proxy request");
    Equal(std::string("bytes=0-"), request.Header("Range"), "reads case-insensitive Range header");
    Check(!ProxyHttp::ParseRequest("POST / HTTP/1.1\r\n\r\n", request), "rejects unsupported proxy method");
    std::string live = ProxyHttp::LiveTsHeaders();
    Check(live.find("Content-Type: video/mpeg\r\n") != std::string::npos
        && live.find("transferMode.dlna.org: Streaming\r\n") != std::string::npos
        && live.find("Content-Length:") == std::string::npos, "formats live MPEG-TS headers without a length");
    Check(ProxyHttp::SrtHeaders(23).find("Content-Length: 23\r\n") != std::string::npos, "formats SRT response headers");
    return TestFailures();
}
