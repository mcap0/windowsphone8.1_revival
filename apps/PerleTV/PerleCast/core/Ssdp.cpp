#include "Ssdp.h"
#include "Util.h"

#include <map>
#include <vector>

namespace PerleCast {

const char* Ssdp::MediaRenderer = "urn:schemas-upnp-org:device:MediaRenderer:1";
const char* Ssdp::AvTransport = "urn:schemas-upnp-org:service:AVTransport:1";

std::string Ssdp::BuildSearchRequest(const std::string& target, int mxSeconds)
{
    return "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: " + std::to_string(mxSeconds) + "\r\n"
        "ST: " + target + "\r\n"
        "USER-AGENT: Windows UPnP/1.1 NewPipe/1.0\r\n\r\n";
}

bool Ssdp::ParseResponse(const std::string& message, SsdpResponse& response)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= message.size()) {
        size_t end = message.find('\n', start);
        if (end == std::string::npos) end = message.size();
        std::string line = Util::Trim(message.substr(start, end - start));
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        lines.push_back(line);
        if (end == message.size()) break;
        start = end + 1;
    }
    if (lines.empty()) return false;
    std::string status = Util::Upper(lines[0]);
    bool answer = status.find("HTTP/") == 0 && status.find(" 200 ") != std::string::npos;
    bool notify = status.find("NOTIFY") == 0;
    if (!answer && !notify) return false;

    std::map<std::string, std::string> headers;
    for (size_t i = 1; i < lines.size(); ++i) {
        size_t colon = lines[i].find(':');
        if (colon != std::string::npos)
            headers[Util::Upper(Util::Trim(lines[i].substr(0, colon)))] = Util::Trim(lines[i].substr(colon + 1));
    }
    std::map<std::string, std::string>::const_iterator nts = headers.find("NTS");
    if (notify && nts != headers.end() && Util::Lower(nts->second) == "ssdp:byebye") return false;
    if (headers.find("LOCATION") == headers.end() || headers["LOCATION"].empty()) return false;
    response.location = headers["LOCATION"];
    response.searchTarget = headers.find("ST") != headers.end() ? headers["ST"]
        : (headers.find("NT") != headers.end() ? headers["NT"] : "");
    response.usn = headers.find("USN") != headers.end() ? headers["USN"] : "";
    response.server = headers.find("SERVER") != headers.end() ? headers["SERVER"] : "";
    return true;
}

} // namespace PerleCast
