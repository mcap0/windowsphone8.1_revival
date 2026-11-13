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
// The private tabs': localStorage in memory only (WebKitLegacy keeps a namespace without a path off the disk).
WebCore::StorageNamespaceProvider& privateStorageNamespaceProvider();
WebCore::DatabaseProvider& databaseProvider();
// lw_suspend: localStorage writes are batched on a timer; write them all now, the process may not come back. The
// function runs (on a storage thread, or here) once they are on disk.
void flushStorage(Function<void()>&& written);
// Everything a site stored: cookies of its host and parent domains it set them on, localStorage, IndexedDB and the
// memory cache of its origin.
void clearSiteData(const WebCore::SecurityOriginData&);
// The last private tab closed: its cookies, passwords, localStorage, IndexedDB, memory cache and permissions go.
void endPrivateSession();
// lw_clear_data: everything of those kinds (lw_clear_kind bits), for every site.
void clearData(unsigned what);

} // namespace Lumia
