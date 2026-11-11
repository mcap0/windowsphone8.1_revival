// WebKit2's process base class, as far as its network cache storage asks it (NetworkCacheStorage.cpp, compiled from
// WebKit's tree for the disk cache): the port is one process, and not the system's WebKit.
#pragma once

namespace WebKit {

struct AuxiliaryProcess {
    static bool isSystemWebKit() { return false; }
};

} // namespace WebKit
