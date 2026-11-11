#include "Didl.h"
#include "Util.h"

namespace PerleCast {

const char* Didl::SrtMimeType = "text/srt";
const char* Didl::DefaultFeatures = "*";

std::string Didl::ProtocolInfo(const std::string& mimeType, const std::string& features)
{
    return "http-get:*:" + mimeType + ":" + (features.empty() ? DefaultFeatures : features);
}

std::string Didl::Build(const CastMedia& media, const std::string& itemId)
{
    std::string title = Util::XmlEscape(media.title);
    std::string uploader = Util::XmlEscape(media.uploader);
    std::string art = media.thumbnailUrl.empty() ? "" : "<upnp:albumArtURI>" + Util::XmlEscape(media.thumbnailUrl) + "</upnp:albumArtURI>";
    std::string duration;
    if (media.durationSeconds > 0) duration = " duration=\"" + Util::FormatTime(media.durationSeconds) + ".000\"";
    std::string caption;
    std::string subtitleRes;
    if (!media.subtitleUrl.empty()) {
        std::string url = Util::XmlEscape(media.subtitleUrl);
        caption = "<sec:CaptionInfoEx sec:type=\"srt\">" + url + "</sec:CaptionInfoEx>"
            "<sec:CaptionInfo sec:type=\"srt\">" + url + "</sec:CaptionInfo>";
        subtitleRes = "<res protocolInfo=\"" + Util::XmlEscape(ProtocolInfo(SrtMimeType)) + "\">" + url + "</res>";
    }
    return "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
        " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
        " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\""
        " xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\""
        + std::string(media.subtitleUrl.empty() ? "" : " xmlns:sec=\"http://www.sec.co.kr/\"") + ">"
        "<item id=\"" + Util::XmlEscape(itemId) + "\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>" + title + "</dc:title>"
        "<dc:creator>" + uploader + "</dc:creator>"
        "<upnp:artist>" + uploader + "</upnp:artist>"
        "<upnp:class>object.item.videoItem</upnp:class>" + art + caption
        + "<res protocolInfo=\"" + Util::XmlEscape(ProtocolInfo(media.mimeType, media.dlnaFeatures)) + "\"" + duration + ">"
        + Util::XmlEscape(media.url) + "</res>" + subtitleRes + "</item></DIDL-Lite>";
}

} // namespace PerleCast
