// Forced into the network cache storage's sources (port/CMakeLists.txt): the log channels WebKit2 declares in its own
// Logging.h, which the port does not build. Defined in DiskCache.cpp, off; RELEASE_LOG_ERROR goes through them.
#pragma once

#include <wtf/Assertions.h>

extern WTFLogChannel LogNetworkCache;
extern WTFLogChannel LogNetworkCacheStorage;
extern WTFLogChannel LogNetworkCacheSpeculativePreloading;
