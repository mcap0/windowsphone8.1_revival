#include "Test.h"
#include "../core/Ssdp.h"

int RunSsdpTests()
{
    using namespace PerleCast;
    std::string search = Ssdp::BuildSearchRequest(Ssdp::MediaRenderer, 3);
    Check(search.find("MX: 3\r\n") != std::string::npos && search.find("ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n") != std::string::npos, "builds M-SEARCH request");
    SsdpResponse response;
    std::string raw = "HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.50:9197/dmr\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSN: uuid:tv\r\nSERVER: Samsung\r\n\r\n";
    Check(Ssdp::ParseResponse(raw, response) && response.location == "http://192.168.1.50:9197/dmr" && response.server == "Samsung", "parses SSDP response headers");
    Check(!Ssdp::ParseResponse("HTTP/1.1 404 Not Found\r\nLOCATION: http://a/b\r\n\r\n", response), "rejects invalid SSDP response");
    return TestFailures();
}
