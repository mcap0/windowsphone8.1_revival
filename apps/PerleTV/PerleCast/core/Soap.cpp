#include "Soap.h"
#include "Util.h"
#include "Xml.h"

#include <cstdlib>
#include <sstream>

namespace PerleCast {

const char* Soap::ContentType = "text/xml; charset=\"utf-8\"";

std::string Soap::BuildEnvelope(const std::string& serviceType, const std::string& action,
                                const std::vector<std::pair<std::string, std::string> >& arguments)
{
    std::string body;
    for (size_t i = 0; i < arguments.size(); ++i)
        body += "<" + arguments[i].first + ">" + Util::XmlEscape(arguments[i].second) + "</" + arguments[i].first + ">";
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" + action + " xmlns:u=\"" + Util::XmlEscape(serviceType) + "\">" + body
        + "</u:" + action + "></s:Body></s:Envelope>";
}

std::string Soap::ActionHeader(const std::string& serviceType, const std::string& action)
{
    return "\"" + serviceType + "#" + action + "\"";
}

std::string Soap::DescribeError(int code)
{
    switch (code) {
    case 401: return "Invalid action";
    case 402: return "Invalid arguments";
    case 500: return "Internal server error";
    case 501: return "Action failed";
    case 701: return "Transition not available";
    case 702: return "No contents";
    case 703: return "Read error";
    case 704: return "Format not supported for playback";
    case 705: return "Transport is locked";
    case 710: return "Seek mode not supported";
    case 711: return "Illegal seek target";
    case 714: return "Illegal MIME-type";
    case 715: return "Content busy";
    case 716: return "Resource not found";
    case 718: return "Invalid InstanceID";
    default: return "Unknown error";
    }
}

SoapResult Soap::ParseResponse(const std::string& action, int httpCode, const std::string& xml)
{
    SoapResult result;
    try {
        XmlNode root = Xml::Parse(xml);
        const XmlNode* fault = root.Find("Fault");
        if (fault || httpCode < 200 || httpCode > 299) {
            const XmlNode* upnp = fault ? fault->Find("UPnPError") : 0;
            if (upnp) {
                const XmlNode* code = upnp->Find("errorCode");
                const XmlNode* desc = upnp->Find("errorDescription");
                if (code) result.errorCode = std::atoi(code->Text().c_str());
                if (desc) result.errorDescription = desc->Text();
            }
            if (!result.errorCode) result.errorCode = httpCode;
            if (result.errorDescription.empty() && fault) {
                const XmlNode* description = fault->Find("faultstring");
                if (description) result.errorDescription = description->Text();
            }
            if (result.errorDescription.empty()) result.errorDescription = DescribeError(result.errorCode);
            return result;
        }
        const XmlNode* response = root.Find(action + "Response");
        if (response) {
            for (size_t i = 0; i < response->children.size(); ++i)
                result.values[response->children[i].name] = response->children[i].Text();
        }
        result.ok = true;
    } catch (...) {
        result.errorCode = httpCode;
        result.errorDescription = httpCode < 200 || httpCode > 299
            ? "HTTP error without valid SOAP body" : "Invalid SOAP answer";
    }
    return result;
}

std::string Soap::BuildSetUri(const std::string& serviceType, const CastMedia& media)
{
    std::vector<std::pair<std::string, std::string> > args;
    args.push_back(std::make_pair("InstanceID", "0"));
    args.push_back(std::make_pair("CurrentURI", media.url));
    args.push_back(std::make_pair("CurrentURIMetaData", Didl::Build(media)));
    return BuildEnvelope(serviceType, "SetAVTransportURI", args);
}

} // namespace PerleCast
