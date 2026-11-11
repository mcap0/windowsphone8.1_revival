#ifndef PERLECAST_DIDL_H
#define PERLECAST_DIDL_H

#include <cstdint>
#include <string>

namespace PerleCast {

struct CastMedia {
    std::string url;
    std::string title;
    std::string uploader;
    std::string mimeType;
    int64_t durationSeconds;
    std::string thumbnailUrl;
    std::string subtitleUrl;
    std::string dlnaFeatures;

    CastMedia() : durationSeconds(-1) {}
};

class Didl {
public:
    static const char* SrtMimeType;
    static const char* DefaultFeatures;
    static std::string ProtocolInfo(const std::string& mimeType, const std::string& features = "");
    static std::string Build(const CastMedia& media, const std::string& itemId = "0");
};

} // namespace PerleCast

#endif
