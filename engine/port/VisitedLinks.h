// Visited links (4c): the addresses the user has been to, shared by every tab, so :visited styles show them. Kept in
// data_directory\visited.dat across runs. Web thread.
#pragma once

#include <WebCore/VisitedLinkStore.h>

namespace Lumia {

WebCore::VisitedLinkStore& visitedLinks();
// lw_suspend: written to disk if it changed.
void saveVisitedLinks();
// lw_clear_visited_links: the app cleared its history.
void clearVisitedLinks();

} // namespace Lumia
