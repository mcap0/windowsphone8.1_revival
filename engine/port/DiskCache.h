// HTTP disk cache (4d): what curl's loads find on disk before going to the network (DiskCache.cpp). Web thread.
#pragma once

#include <wtf/Forward.h>

namespace Lumia::DiskCache {

// Opens the cache in lw_config.cache_directory and puts it in front of curl. After lw_init, on the Web thread.
void initialize();
// "Cancella dati del sito": every entry whose host is `domain` or one of its subdomains.
void removeSite(const String& domain);
// "memoria": how big it is, as a log line.
String describe();

} // namespace Lumia::DiskCache
