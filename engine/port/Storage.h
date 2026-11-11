// Web storage of the port: localStorage and sessionStorage (WebKitLegacy's StorageNamespaceImpl, SQLite files in
// data_directory\LocalStorage) and IndexedDB (WebKitLegacy's InProcessIDBServer, data_directory\IndexedDB), the same
// in-process implementations WebKitLegacy uses. Web thread.
#pragma once

#include <wtf/Forward.h>

namespace WebCore {
class DatabaseProvider;
class SecurityOriginData;
class StorageNamespaceProvider;
}

namespace Lumia {

// Once, before the first page.
void initializeStorage();
// Shared by every tab.
WebCore::StorageNamespaceProvider& storageNamespaceProvider();
WebCore::DatabaseProvider& databaseProvider();
// lw_suspend: localStorage writes are batched on a timer; write them all now, the process may not come back. The
// function runs (on a storage thread, or here) once they are on disk.
void flushStorage(Function<void()>&& written);
// Everything a site stored: cookies of its host and parent domains it set them on, localStorage, IndexedDB and the
// memory cache of its origin.
void clearSiteData(const WebCore::SecurityOriginData&);

} // namespace Lumia
