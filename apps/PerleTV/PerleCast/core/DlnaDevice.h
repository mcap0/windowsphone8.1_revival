#ifndef PERLECAST_DLNDEVICE_H
#define PERLECAST_DLNDEVICE_H

#include <string>

namespace PerleCast {

struct DlnaDevice {
    std::string udn;
    std::string friendlyName;
    std::string manufacturer;
    std::string modelName;
    std::string location;
    std::string avTransportControlUrl;
    std::string avTransportServiceType;
    std::string renderingControlUrl;
    std::string renderingControlServiceType;
    std::string connectionManagerUrl;
    std::string connectionManagerServiceType;

    std::string Brand() const;
    std::string Label() const;
};

class DeviceDescriptionParser {
public:
    static bool Parse(const std::string& xml, const std::string& location, DlnaDevice& device);
    static std::string ResolveUrl(const std::string& base, const std::string& url);
};

} // namespace PerleCast

#endif
