#include "CastController.h"
#include "Soap.h"
#include "Util.h"

#include <cstdlib>

namespace PerleCast {

CastController::CastController(ICastNet& network, const DlnaDevice& device)
    : network_(network), device_(device)
{
    state_.deviceName = device.friendlyName;
}

const CastState& CastController::State() const { return state_; }

std::string CastController::StatusText(CastStatus status)
{
    switch (status) {
    case Connecting: return "Connessione\xE2\x80\xA6";
    case Buffering: return "Caricamento sul TV\xE2\x80\xA6";
    case Playing: return "In riproduzione";
    case Paused: return "In pausa";
    case Stopped: return "Fermato";
    default: return "Errore";
    }
}

SoapResult CastController::Call(const std::string& url, const std::string& service, const std::string& action,
                                const std::vector<std::pair<std::string, std::string> >& args)
{
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = Soap::ContentType;
    headers["SOAPACTION"] = Soap::ActionHeader(service, action);
    HttpResult response = network_.Post(url, headers, Soap::BuildEnvelope(service, action, args));
    return Soap::ParseResponse(action, response.code, response.body);
}

bool CastController::SetError(const SoapResult& result)
{
    state_.active = false;
    state_.status = Error;
    state_.errorCode = result.errorCode;
    state_.error = result.errorDescription.empty() ? Soap::DescribeError(result.errorCode) : result.errorDescription;
    return false;
}

bool CastController::Action(const std::string& action, const std::vector<std::pair<std::string, std::string> >& args)
{
    SoapResult result = Call(device_.avTransportControlUrl, device_.avTransportServiceType, action, args);
    if (!result.ok) return SetError(result);
    return true;
}

bool CastController::Start(const CastMedia& media)
{
    return SetUri(media) && Play();
}

bool CastController::SetUri(const CastMedia& media)
{
    state_.active = true;
    state_.status = Connecting;
    state_.title = media.title;
    state_.positionSeconds = 0;
    state_.durationSeconds = media.durationSeconds;
    state_.error.clear();
    state_.errorCode = 0;
    SoapResult result = Call(device_.avTransportControlUrl, device_.avTransportServiceType, "SetAVTransportURI",
        std::vector<std::pair<std::string, std::string> >{
            std::make_pair("InstanceID", "0"), std::make_pair("CurrentURI", media.url),
            std::make_pair("CurrentURIMetaData", Didl::Build(media))});
    if (!result.ok) return SetError(result);
    state_.status = Buffering;
    return true;
}

bool CastController::Play()
{
    bool ok = Action("Play", std::vector<std::pair<std::string, std::string> >{
        std::make_pair("InstanceID", "0"), std::make_pair("Speed", "1")});
    if (ok) { state_.active = true; state_.status = Buffering; state_.error.clear(); state_.errorCode = 0; }
    return ok;
}

bool CastController::Pause()
{
    bool ok = Action("Pause", std::vector<std::pair<std::string, std::string> >{std::make_pair("InstanceID", "0")});
    if (ok) state_.status = Paused;
    return ok;
}

bool CastController::Stop()
{
    bool ok = Action("Stop", std::vector<std::pair<std::string, std::string> >{std::make_pair("InstanceID", "0")});
    state_.active = false;
    if (ok) state_.status = Stopped;
    return ok;
}

bool CastController::Seek(int64_t positionSeconds)
{
    if (positionSeconds < 0) return false;
    bool ok = Action("Seek", std::vector<std::pair<std::string, std::string> >{
        std::make_pair("InstanceID", "0"), std::make_pair("Unit", "REL_TIME"),
        std::make_pair("Target", Util::FormatTime(positionSeconds))});
    if (ok) state_.positionSeconds = positionSeconds;
    return ok;
}

bool CastController::Poll()
{
    if (!state_.active) return false;
    std::vector<std::pair<std::string, std::string> > args(1, std::make_pair("InstanceID", "0"));
    SoapResult transport = Call(device_.avTransportControlUrl, device_.avTransportServiceType, "GetTransportInfo", args);
    if (!transport.ok) return SetError(transport);
    SoapResult position = Call(device_.avTransportControlUrl, device_.avTransportServiceType, "GetPositionInfo", args);
    if (!position.ok) return SetError(position);

    std::map<std::string, std::string>::const_iterator state = transport.values.find("CurrentTransportState");
    std::string value = state == transport.values.end() ? "" : Util::Upper(state->second);
    if (value == "PLAYING") state_.status = Playing;
    else if (value == "PAUSED_PLAYBACK" || value == "PAUSED_RECORDING") state_.status = Paused;
    else if (value == "STOPPED" || value == "NO_MEDIA_PRESENT") state_.status = Stopped;
    else if (value == "TRANSITIONING") state_.status = Buffering;

    std::map<std::string, std::string>::const_iterator rel = position.values.find("RelTime");
    std::map<std::string, std::string>::const_iterator dur = position.values.find("TrackDuration");
    if (rel != position.values.end()) state_.positionSeconds = Util::ParseTime(rel->second);
    if (dur != position.values.end()) state_.durationSeconds = Util::ParseTime(dur->second);
    return true;
}

bool CastController::SetVolume(int volume)
{
    std::vector<std::pair<std::string, std::string> > args;
    args.push_back(std::make_pair("InstanceID", "0"));
    args.push_back(std::make_pair("Channel", "Master"));
    args.push_back(std::make_pair("DesiredVolume", std::to_string(volume)));
    SoapResult result = Call(device_.renderingControlUrl, device_.renderingControlServiceType, "SetVolume", args);
    return result.ok ? true : SetError(result);
}

bool CastController::GetVolume(int& volume)
{
    std::vector<std::pair<std::string, std::string> > args;
    args.push_back(std::make_pair("InstanceID", "0"));
    args.push_back(std::make_pair("Channel", "Master"));
    SoapResult result = Call(device_.renderingControlUrl, device_.renderingControlServiceType, "GetVolume", args);
    if (!result.ok) return SetError(result);
    std::map<std::string, std::string>::const_iterator found = result.values.find("CurrentVolume");
    if (found == result.values.end()) return false;
    volume = std::atoi(found->second.c_str());
    return true;
}

} // namespace PerleCast
