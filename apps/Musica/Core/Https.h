// Blocking HTTPS GET with TsNet's mbedTLS stream, for the sites WP8.1's own TLS cannot reach: open.spotify.com only
// offers ECDHE-RSA with AES-GCM / ChaCha20, which Windows Phone 8.1 does not have. Portable C++11 (PC test too).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace online {

// url: https://host/path. Follows up to 3 redirects. Returns the body ("" and err set on failure).
std::string httpsGet(const std::string& url, const std::vector<std::pair<std::string, std::string> >& headers,
                     std::string& err, int timeoutMs = 20000);

}  // namespace online
