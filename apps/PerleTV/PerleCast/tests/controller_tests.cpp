#include "Test.h"
#include "../core/CastController.h"
#include "../core/Soap.h"

#include <map>

namespace {
class FakeTv : public PerleCast::ICastNet {
public:
    std::string transport;
    std::string relTime;
    bool failPlay;
    FakeTv() : transport("PLAYING"), relTime("0:00:42"), failPlay(false) {}

    PerleCast::HttpResult Post(const std::string&, const std::map<std::string, std::string>& headers, const std::string& body)
    {
        PerleCast::HttpResult result;
        result.code = 200;
        std::string action;
        std::map<std::string, std::string>::const_iterator it = headers.find("SOAPACTION");
        if (it != headers.end()) {
            size_t hash = it->second.find('#');
            size_t end = it->second.find('"', hash);
            action = it->second.substr(hash + 1, end - hash - 1);
        }
        if (failPlay && action == "Play") {
            result.code = 500;
            result.body = "<Envelope><Fault><detail><UPnPError><errorCode>701</errorCode><errorDescription>Transition not available</errorDescription></UPnPError></detail></Fault></Envelope>";
            return result;
        }
        if (action == "GetTransportInfo")
            result.body = "<Envelope><GetTransportInfoResponse><CurrentTransportState>" + transport + "</CurrentTransportState></GetTransportInfoResponse></Envelope>";
        else if (action == "GetPositionInfo")
            result.body = "<Envelope><GetPositionInfoResponse><RelTime>" + relTime + "</RelTime><TrackDuration>0:10:00</TrackDuration></GetPositionInfoResponse></Envelope>";
        else if (action == "SetAVTransportURI" || action == "Play" || action == "Pause" || action == "Stop" || action == "Seek")
            result.body = "<Envelope><" + action + "Response/></Envelope>";
        else result.body = "<Envelope><" + action + "Response/></Envelope>";
        (void)body;
        return result;
    }

    PerleCast::HttpResult Get(const std::string&) { PerleCast::HttpResult r; r.code = 404; return r; }
};
}

int RunControllerTests()
{
    using namespace PerleCast;
    DlnaDevice device;
    device.friendlyName = "Fake TV";
    device.avTransportControlUrl = "http://tv/av";
    device.avTransportServiceType = "urn:schemas-upnp-org:service:AVTransport:1";
    device.renderingControlUrl = "http://tv/rc";
    device.renderingControlServiceType = "urn:schemas-upnp-org:service:RenderingControl:1";

    FakeTv tv;
    CastController controller(tv, device);
    CastMedia media;
    media.url = "http://phone/video.mp4";
    media.title = "Video";
    media.uploader = "Channel";
    media.mimeType = "video/mp4";
    media.durationSeconds = 600;
    Check(controller.Start(media), "controller starts cast and issues Play");
    Check(controller.Poll() && controller.State().status == Playing && controller.State().positionSeconds == 42, "controller polls TV state and position");
    Check(controller.Pause() && controller.State().status == Paused, "controller pauses");
    Check(controller.Seek(90) && controller.State().positionSeconds == 90, "controller seeks with REL_TIME");
    Check(controller.Stop() && controller.State().status == Stopped, "controller stops");

    FakeTv failedTv;
    failedTv.failPlay = true;
    CastController failed(failedTv, device);
    Check(!failed.Start(media) && failed.State().status == Error && failed.State().error == "Transition not available", "UPnP fault transitions controller to Error");
    Equal(std::string("In riproduzione"), CastController::StatusText(Playing), "Italian controller status string");
    return TestFailures();
}
