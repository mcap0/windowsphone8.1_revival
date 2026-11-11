// JavaScript bytecode cache on disk (PLAN.md §56): what JSC compiled for a page's scripts comes back from the flash on
// the next visit instead of being parsed and compiled again. WebCore's side is WebCore/bindings/js/
// ScriptBytecodeCacheWP81.h (the source providers' hooks); this file is the storage and the policy. Web thread.
#pragma once

#include <wtf/Function.h>
#include <wtf/Seconds.h>
#include <wtf/text/WTFString.h>

namespace Lumia::BytecodeCache {

// lw_init: the files live in <cache directory>\JSBytecode\<webkit.dll build>; other builds' folders go.
void initialize();
// The app may not come back (suspension, memory pressure): the scripts' functions compiled since their last commit
// that are worth a write (BytecodeCache.cpp, worthWriting) go to the flash, on a background queue; `done` runs on the
// Web thread once the files are written (lw_suspend holds the app's suspension deferral until then).
void commit(const char* reason, Function<void()>&& done = nullptr);
// A main-frame load finished: commit once the page has run its start-up code, unless another load comes first.
void scheduleCommit(Seconds delay);
// "Cancella dati": every file, and the scripts seen once.
void clear();
// "memoria": files on the flash and what they did this run, as one log line.
String describe();
// "bytecode" in the address box: off (scripts run from now on neither read nor write files) or on again; true when on.
bool toggle();

} // namespace Lumia::BytecodeCache
