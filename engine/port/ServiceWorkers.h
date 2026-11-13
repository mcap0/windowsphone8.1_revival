// Service workers in one process. WebKit2 keeps the server (SWServer: registrations, jobs, worker states) in the
// network process, the workers in a web process of their own and the pages in theirs, with IPC between the three.
// Here all three are WebCore's own classes on the Web thread (the workers on their own threads, as WebCore runs
// them), and the four connection classes of ServiceWorkers.cpp call each other directly, one run loop turn later
// where WebKit2 had a message.
//
// A page's loads reach the worker through ResourceLoader's load interceptor (ResourceLoader.cpp, WP81): the port's
// equivalent of the network process's ServiceWorkerFetchTask.
#pragma once

#include <wtf/CompletionHandler.h>
#include <wtf/Forward.h>

namespace WebCore {
class ResourceLoader;
class SecurityOriginData;
}

namespace Lumia {

// Once, on the Web thread, before the first page.
void initializeServiceWorkers();
// The loader strategy's remove(): a load answered by a worker was cancelled or ended.
void serviceWorkerLoadRemoved(WebCore::ResourceLoader&);
// Registrations (and their workers) of one top origin, or of every site.
void clearServiceWorkers(const WebCore::SecurityOriginData&, CompletionHandler<void()>&&);
void clearAllServiceWorkers(CompletionHandler<void()>&&);
// lw_suspend: registrations waiting to be written.
void flushServiceWorkers(CompletionHandler<void()>&&);
// "motore sw": one line per registration and running worker, for the log.
String describeServiceWorkers();

} // namespace Lumia
