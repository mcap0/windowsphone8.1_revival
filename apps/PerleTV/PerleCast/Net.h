#ifndef PERLECAST_NET_H
#define PERLECAST_NET_H

#include "core/CastController.h"

namespace PerleCast {

class Net : public ICastNet
{
public:
    virtual HttpResult Post(const std::string& url,
                            const std::map<std::string, std::string>& headers,
                            const std::string& body);
    virtual HttpResult Get(const std::string& url);
};

} // namespace PerleCast

#endif
