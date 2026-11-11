#pragma once

#include <wtf/text/WTFString.h>

namespace Lumia {

// Ad and tracker requests stopped before they start (PLAN.md §56.5 point 6, §74). Web thread only, but installBlocker.
void installBlocker();
// "blocco": everything off or on again; "blocco <host>": that site's pages are left alone (or blocked again).
void blockerCommand(const String& argument);

} // namespace Lumia
