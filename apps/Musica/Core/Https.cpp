#include "Https.h"

#include <cstdlib>

#include "tsio.h"

namespace online {

namespace {
std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
    return s;
}

std::string header(const std::string& head, const std::string& name)
{
    std::string h = lower(head), n = "\r\n" + lower(name) + ":";
    size_t p = h.find(n);
    if (p == std::string::npos) return "";
    p += n.size();
    size_t e = head.find("\r\n", p);
    std::string v = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    size_t b = v.find_first_not_of(" \t");
    return b == std::string::npos ? "" : v.substr(b);
}

bool dechunk(const std::string& in, std::string& out)
{
    out.clear();
    size_t p = 0;
    while (p < in.size()) {
        size_t e = in.find("\r\n", p);
        if (e == std::string::npos) return false;
        unsigned long n = std::strtoul(in.substr(p, e - p).c_str(), 0, 16);
        p = e + 2;
        if (n == 0) return true;
        if (p + n > in.size()) return false;
        out.append(in, p, n);
        p += n + 2;
    }
    return false;
}
}

std::string httpsGet(const std::string& url0, const std::vector<std::pair<std::string, std::string> >& headers,
                     std::string& err, int timeoutMs)
{
    std::string url = url0;
    for (int hop = 0; hop < 4; ++hop) {
        if (url.compare(0, 8, "https://") != 0) { err = "solo https"; return ""; }
        size_t slash = url.find('/', 8);
        std::string host = url.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
        std::string path = slash == std::string::npos ? "/" : url.substr(slash);
        std::unique_ptr<ts::TlsStream> s = ts::TlsStream::connect(host, 443, timeoutMs, err);
        if (!s) return "";
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
        for (size_t i = 0; i < headers.size(); ++i) req += headers[i].first + ": " + headers[i].second + "\r\n";
        req += "\r\n";
        if (!s->write(req)) { err = "invio non riuscito"; return ""; }
        std::string head;
        if (!ts::readHttpHead(*s, head, timeoutMs)) { err = "nessuna risposta da " + host; return ""; }
        int status = std::atoi(head.c_str() + (head.find(' ') == std::string::npos ? 0 : head.find(' ') + 1));
        std::string location = header(head, "Location");
        if (status >= 300 && status < 400 && !location.empty()) {
            url = location[0] == '/' ? "https://" + host + location : location;
            continue;
        }
        std::string body;
        long long want = header(head, "Content-Length").empty() ? -1 : std::atoll(header(head, "Content-Length").c_str());
        int64_t end = ts::nowMs() + timeoutMs;
        uint8_t buf[16384];
        while (want < 0 || (long long)body.size() < want) {
            int left = (int)(end - ts::nowMs());
            if (left <= 0) { err = "tempo scaduto"; return ""; }
            int n = s->read(buf, sizeof buf, left);
            if (n == 0) break;
            if (n > 0) body.append((const char*)buf, n);
        }
        if (lower(header(head, "Transfer-Encoding")).find("chunked") != std::string::npos) {
            std::string plain;
            if (!dechunk(body, plain)) { err = "risposta incompleta"; return ""; }
            body.swap(plain);
        }
        if (status != 200) { err = "HTTP " + std::to_string((long long)status); return ""; }
        return body;
    }
    err = "troppi reindirizzamenti";
    return "";
}

}  // namespace online
