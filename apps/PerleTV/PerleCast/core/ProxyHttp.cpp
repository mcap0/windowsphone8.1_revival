#include "ProxyHttp.h"
#include "Util.h"

#include <sstream>

namespace PerleCast {

std::string HttpRequest::Header(const std::string& name) const
{
    std::map<std::string, std::string>::const_iterator it = headers.find(Util::Lower(name));
    return it == headers.end() ? "" : it->second;
}

bool ProxyHttp::ParseRequest(const std::string& raw, HttpRequest& request)
{
    size_t end = raw.find("\r\n");
    if (end == std::string::npos) end = raw.find('\n');
    if (end == std::string::npos) return false;
    std::istringstream line(raw.substr(0, end));
    if (!(line >> request.method >> request.path >> request.version)) return false;
    request.method = Util::Upper(request.method);
    if (request.method != "GET" && request.method != "HEAD") return false;
    size_t pos = raw.find('\n', end);
    if (pos == std::string::npos) return true;
    ++pos;
    while (pos < raw.size()) {
        size_t next = raw.find('\n', pos);
        if (next == std::string::npos) next = raw.size();
        std::string value = raw.substr(pos, next - pos);
        if (!value.empty() && value[value.size() - 1] == '\r') value.erase(value.size() - 1);
        if (value.empty()) break;
        size_t colon = value.find(':');
        if (colon != std::string::npos)
            request.headers[Util::Lower(Util::Trim(value.substr(0, colon)))] = Util::Trim(value.substr(colon + 1));
        pos = next + 1;
    }
    return true;
}

std::string ProxyHttp::LiveTsHeaders(const std::string& profile)
{
    const std::string flags = "DLNA.ORG_OP=00;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01700000000000000000000000000000";
    std::string features = profile.empty() ? flags : "DLNA.ORG_PN=" + profile + ";" + flags;
    return "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mpeg\r\n"
        "transferMode.dlna.org: Streaming\r\n"
        "contentFeatures.dlna.org: " + features + "\r\n"
        "Server: Windows UPnP/1.0 NewPipe-Cast/1.0\r\n"
        "Connection: close\r\n\r\n";
}

std::string ProxyHttp::SrtHeaders(size_t length)
{
    std::ostringstream out;
    out << "HTTP/1.1 200 OK\r\nContent-Type: text/srt\r\nContent-Length: " << length
        << "\r\nServer: Windows UPnP/1.0 NewPipe-Cast/1.0\r\nConnection: close\r\n\r\n";
    return out.str();
}

std::string ProxyHttp::SimpleResponse(int code, const std::string& reason, const std::string& body)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << reason << "\r\nContent-Length: " << body.size()
        << "\r\nConnection: close\r\n\r\n" << body;
    return out.str();
}

} // namespace PerleCast
