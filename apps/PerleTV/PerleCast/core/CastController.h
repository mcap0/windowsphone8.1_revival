#ifndef PERLECAST_CASTCONTROLLER_H
#define PERLECAST_CASTCONTROLLER_H

#include "DlnaDevice.h"
#include "Didl.h"
#include "ProxyHttp.h"
#include "Soap.h"

#include <map>
#include <string>
#include <vector>

namespace PerleCast {

struct ICastNet {
    virtual ~ICastNet() {}
    virtual HttpResult Post(const std::string& url, const std::map<std::string, std::string>& headers,
                            const std::string& body) = 0;
    virtual HttpResult Get(const std::string& url) = 0;
};

enum CastStatus { Connecting, Buffering, Playing, Paused, Stopped, Error };

struct CastState {
    bool active;
    std::string deviceName;
    std::string title;
    CastStatus status;
    int64_t positionSeconds;
    int64_t durationSeconds;
    std::string error;
    int errorCode;  // UPnP fault code of the last failed action, 0 when none
    CastState() : active(false), status(Connecting), positionSeconds(-1), durationSeconds(-1), errorCode(0) {}
};

class CastController {
public:
    CastController(ICastNet& network, const DlnaDevice& device);
    const CastState& State() const;
    static std::string StatusText(CastStatus status);
    bool Start(const CastMedia& media);
    bool SetUri(const CastMedia& media);   // SetAVTransportURI only (Start = SetUri + Play)
    bool Play();
    bool Pause();
    bool Stop();
    bool Seek(int64_t positionSeconds);
    bool Poll();
    bool SetVolume(int volume);
    bool GetVolume(int& volume);

private:
    ICastNet& network_;
    DlnaDevice device_;
    CastState state_;
    bool Action(const std::string& action, const std::vector<std::pair<std::string, std::string> >& args);
    SoapResult Call(const std::string& url, const std::string& service, const std::string& action,
                    const std::vector<std::pair<std::string, std::string> >& args);
    bool SetError(const SoapResult& result);
};

} // namespace PerleCast

#endif
