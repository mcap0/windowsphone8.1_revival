#ifndef PERLECAST_SOAP_H
#define PERLECAST_SOAP_H

#include "Didl.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace PerleCast {

struct SoapResult {
    bool ok;
    int errorCode;
    std::string errorDescription;
    std::map<std::string, std::string> values;
    SoapResult() : ok(false), errorCode(0) {}
};

class Soap {
public:
    static const char* ContentType;
    static std::string BuildEnvelope(const std::string& serviceType, const std::string& action,
                                     const std::vector<std::pair<std::string, std::string> >& arguments);
    static std::string ActionHeader(const std::string& serviceType, const std::string& action);
    static SoapResult ParseResponse(const std::string& action, int httpCode, const std::string& xml);
    static std::string BuildSetUri(const std::string& serviceType, const CastMedia& media);
    static std::string DescribeError(int code);
};

} // namespace PerleCast

#endif
