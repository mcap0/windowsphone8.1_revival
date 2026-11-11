#ifndef PERLECAST_DISCOVERY_H
#define PERLECAST_DISCOVERY_H

#include "core/DlnaDevice.h"
#include <vector>

namespace PerleCast {

class Discovery
{
public:
    static std::vector<DlnaDevice> FindRenderers();
};

} // namespace PerleCast

#endif
