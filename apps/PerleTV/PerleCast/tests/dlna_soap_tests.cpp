#include "Test.h"
#include "../core/DlnaDevice.h"
#include "../core/Didl.h"
#include "../core/Soap.h"
#include "../core/Util.h"

int RunDlnaSoapTests()
{
    using namespace PerleCast;
    DlnaDevice device;
    const std::string xml = "<root xmlns=\"urn:test\"><URLBase>http://192.168.1.50:9197/</URLBase><device><friendlyName>Tom &amp; Jerry&apos;s TV</friendlyName><manufacturer>Samsung Electronics</manufacturer><modelName>UE43</modelName><UDN>uuid:tv</UDN><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>/upnp/avt</controlURL></service><service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><controlURL>/upnp/rc</controlURL></service></serviceList></device></root>";
    Check(DeviceDescriptionParser::Parse(xml, "http://192.168.1.50:9197/dmr", device), "parses device description");
    Equal(std::string("Tom & Jerry's TV"), device.friendlyName, "decodes XML entities");
    Equal(std::string("http://192.168.1.50:9197/upnp/avt"), device.avTransportControlUrl, "resolves AVTransport control URL");
    Equal(std::string("Samsung"), device.Brand(), "derives manufacturer brand");

    CastMedia media;
    media.url = "http://192.168.1.20:41234/v/a.mp4";
    media.title = "Tom & Jerry <Live>";
    media.uploader = "Ben & Co";
    media.mimeType = "video/mp4";
    media.durationSeconds = 3725;
    media.subtitleUrl = "http://192.168.1.20/v/a.srt";
    std::string didl = Didl::Build(media);
    Check(didl.find("Tom &amp; Jerry &lt;Live&gt;") != std::string::npos, "DIDL escapes title and metadata");
    Check(didl.find("sec:CaptionInfoEx sec:type=\"srt\"") != std::string::npos, "DIDL announces Samsung subtitles");

    std::vector<std::pair<std::string, std::string> > args;
    args.push_back(std::make_pair("InstanceID", "0"));
    args.push_back(std::make_pair("Target", "0:01:30"));
    std::string envelope = Soap::BuildEnvelope(device.avTransportServiceType, "Seek", args);
    Check(envelope.find("<InstanceID>0</InstanceID><Target>0:01:30</Target>") != std::string::npos, "SOAP preserves argument order");
    Equal(std::string("\"urn:schemas-upnp-org:service:AVTransport:1#Play\""),
          Soap::ActionHeader(device.avTransportServiceType, "Play"), "formats SOAPACTION header");

    std::string answer = "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:GetPositionInfoResponse xmlns:u=\"urn:x\"><TrackDuration>0:04:10</TrackDuration><RelTime>0:01:02</RelTime></u:GetPositionInfoResponse></s:Body></s:Envelope>";
    SoapResult parsed = Soap::ParseResponse("GetPositionInfo", 200, answer);
    Check(parsed.ok && parsed.values["RelTime"] == "0:01:02", "parses SOAP response values");
    std::string fault = "<Envelope><Fault><faultstring>UPnPError</faultstring><detail><UPnPError><errorCode>714</errorCode><errorDescription>Illegal MIME-type</errorDescription></UPnPError></detail></Fault></Envelope>";
    SoapResult failed = Soap::ParseResponse("Play", 500, fault);
    Check(!failed.ok && failed.errorCode == 714 && failed.errorDescription == "Illegal MIME-type", "parses UPnP fault");
    return TestFailures();
}
