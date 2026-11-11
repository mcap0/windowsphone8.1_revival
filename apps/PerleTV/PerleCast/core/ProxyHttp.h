#ifndef PERLECAST_PROXYHTTP_H
#define PERLECAST_PROXYHTTP_H

#include <map>
#include <string>

namespace PerleCast {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string Header(const std::string& name) const;
};

struct HttpResult {
    int code;
    std::string body;
    std::map<std::string, std::string> headers;
    HttpResult() : code(0) {}
};

class ProxyHttp {
public:
    static bool ParseRequest(const std::string& raw, HttpRequest& request);
    static std::string LiveTsHeaders(const std::string& profile = "");
    static std::string SrtHeaders(size_t length);
    static std::string SimpleResponse(int code, const std::string& reason, const std::string& body = "");
};

} // namespace PerleCast

#endif
