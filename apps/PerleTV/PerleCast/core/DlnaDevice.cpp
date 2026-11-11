#include "DlnaDevice.h"
#include "Util.h"
#include "Xml.h"

#include <algorithm>

namespace PerleCast {

std::string DlnaDevice::Brand() const
{
    std::string brand = Util::Trim(manufacturer);
    size_t space = brand.find(' ');
    if (space != std::string::npos) brand.resize(space);
    while (!brand.empty() && (brand[brand.size() - 1] == '.' || brand[brand.size() - 1] == ',')) brand.erase(brand.size() - 1);
    if (brand.empty() || Util::Lower(friendlyName).find(Util::Lower(brand)) != std::string::npos) return "";
    return brand;
}

std::string DlnaDevice::Label() const
{
    std::string brand = Brand();
    return brand.empty() ? friendlyName : friendlyName + " (" + brand + ")";
}

namespace {
std::string ChildText(const XmlNode& node, const std::string& child)
{
    const XmlNode* found = node.Child(child);
    return found ? found->Text() : "";
}

bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.compare(0, prefix.size(), prefix) == 0;
}

std::string ServiceValue(const XmlNode& service, const std::string& key)
{
    return ChildText(service, key);
}
}

bool DeviceDescriptionParser::Parse(const std::string& xml, const std::string& location, DlnaDevice& device)
{
    XmlNode root = Xml::Parse(xml);
    const XmlNode* rootDevice = root.Find("device");
    if (!rootDevice) return false;

    std::vector<const XmlNode*> devices;
    devices.push_back(rootDevice);
    for (size_t i = 0; i < rootDevice->children.size(); ++i) {
        if (rootDevice->children[i].name == "deviceList") {
            const XmlNode& list = rootDevice->children[i];
            for (size_t j = 0; j < list.children.size(); ++j)
                if (list.children[j].name == "device") devices.push_back(&list.children[j]);
        }
    }

    const XmlNode* chosen = 0;
    std::string avt, rc, cm;
    for (size_t i = 0; i < devices.size(); ++i) {
        const XmlNode* list = devices[i]->Child("serviceList");
        if (!list) continue;
        for (size_t j = 0; j < list->children.size(); ++j) {
            const XmlNode& service = list->children[j];
            std::string type = ServiceValue(service, "serviceType");
            if (StartsWith(type, "urn:schemas-upnp-org:service:AVTransport:")) { chosen = devices[i]; avt = type; }
        }
        if (chosen == devices[i]) {
            for (size_t j = 0; j < list->children.size(); ++j) {
                const XmlNode& service = list->children[j];
                std::string type = ServiceValue(service, "serviceType");
                if (StartsWith(type, "urn:schemas-upnp-org:service:RenderingControl:")) rc = type;
                if (StartsWith(type, "urn:schemas-upnp-org:service:ConnectionManager:")) cm = type;
            }
            break;
        }
    }
    if (!chosen) return false;

    std::string base = ChildText(root, "URLBase");
    if (base.empty()) base = location;
    const XmlNode* list = chosen->Child("serviceList");
    std::string avtUrl, rcUrl, cmUrl;
    for (size_t i = 0; list && i < list->children.size(); ++i) {
        const XmlNode& service = list->children[i];
        std::string type = ServiceValue(service, "serviceType");
        std::string control = ServiceValue(service, "controlURL");
        if (StartsWith(type, "urn:schemas-upnp-org:service:AVTransport:")) avtUrl = control;
        else if (StartsWith(type, "urn:schemas-upnp-org:service:RenderingControl:")) rcUrl = control;
        else if (StartsWith(type, "urn:schemas-upnp-org:service:ConnectionManager:")) cmUrl = control;
    }
    if (avtUrl.empty()) return false;

    device.udn = ChildText(*chosen, "UDN");
    if (device.udn.empty()) device.udn = location;
    device.friendlyName = ChildText(*chosen, "friendlyName");
    if (device.friendlyName.empty()) device.friendlyName = Util::UrlHost(location);
    device.manufacturer = ChildText(*chosen, "manufacturer");
    device.modelName = ChildText(*chosen, "modelName");
    device.location = location;
    device.avTransportServiceType = avt;
    device.avTransportControlUrl = ResolveUrl(base, avtUrl);
    device.renderingControlServiceType = rc;
    device.renderingControlUrl = rcUrl.empty() ? "" : ResolveUrl(base, rcUrl);
    device.connectionManagerServiceType = cm;
    device.connectionManagerUrl = cmUrl.empty() ? "" : ResolveUrl(base, cmUrl);
    return true;
}

std::string DeviceDescriptionParser::ResolveUrl(const std::string& base, const std::string& url)
{
    return Util::ResolveUrl(base, url);
}

} // namespace PerleCast
