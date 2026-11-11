// The engine as the port's own code sees it: the lw_config the app passed to lw_init and the events sent back to it.
// Events are delivered through lw_config.callback on the calling thread; strings live only during the call.
#pragma once

#include <wtf/MonotonicTime.h>
#include "lumiaweb.h"
#include <optional>
#include <wtf/URL.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class FileChooser;
class Page;
}

namespace WebKit {
class WebPage;
}

namespace Lumia {

const lw_config& config();

void postEvent(lw_view&, lw_event_type, int64_t number = 0, const String& text = { }, const String& text2 = { }, int id = 0);
void postConsoleMessage(lw_view&, int level, const String& message, const String& source);
// LW_EVENT_FIELD_FOCUS / LW_EVENT_FIELD_UPDATE: `field.text` is set from `text`, which therefore lives in the caller's
// frame and stays valid for the duration of the callback only (lumiaweb.h).
void postFieldEvent(lw_view&, lw_event_type, const lw_field& field, const String& text);

// media/VideoProbe.cpp: the compositor's thread, its context current, once.
void probeSharedVideoTextures();

// Diagnostics from any thread (engine checkpoints, WebKit's own log lines): LW_EVENT_TRACE with no view.
void log(const char* format, ...);

// The user touched the screen or typed (any thread), and when that last happened: what must answer at once is not
// paced (LayerTreeHost::loadPacingDelay).
void noteUserInput();
MonotonicTime lastUserInput();
// "motore ritmo": that pacing off or on (on at start).
bool loadPacingEnabled();
void setLoadPacingEnabled(bool);

// lw_trace_enable: the latency tracer is on (input hops are logged as LW_EVENT_TRACE).
bool traceEnabled();

// lw_gpu_test: records a 6 s frame timeline (timelineMark) and logs it ("linea:" lines, tools/timeline.py).
void startGPUTest();
// Any thread, wait-free; does nothing unless a recording is running. Kinds: p finger event (input thread),
// q its handling on the scrolling thread, r composition requested by the scrolling thread, m by the main thread,
// t frame clock tick, f requested frame started, b render begins, d draw done (before swap), w swap returned,
// c frame complete; g/h glFinish before the scene update (recording only: GPU still busy with the last frame),
// u scene update done (tile uploads).
void timelineMark(char kind);
bool timelineRecording();

// Runs task on the Web thread (queued until it has started). input = true puts it on the run loop's input
// lane: it runs before queued page work instead of behind it (taps, key events).
void postToWebThread(Function<void()>&&, bool input = false);
// Web thread: the view's page, or null once lw_view_destroy has run.
WebKit::WebPage* liveWebPage(lw_view*);

// The User-Agent for a load in this view: the view's own (lw_view_set_user_agent), else the site's
// (lw_set_site_user_agent), else the default (lw_config.user_agent). navigatorPlatform: what navigator.platform says
// with it (null: the engine's default).
String userAgent(const lw_view&, const URL&);
String navigatorPlatform(const lw_view&, const URL&);

// Web thread: the view is on the screen and the app runs, so the user can be asked something.
bool isViewActive(const lw_view&);

// Web thread: shows the page's dialog through LW_EVENT_DIALOG and waits for lw_dialog_reply. nullopt = cancelled (or
// dismissed because the tab cannot show it), else the value (empty but for prompts).
std::optional<String> runDialog(lw_view&, lw_dialog_kind, const String& message, const String& defaultValue);

// Web thread: window.open. Asks the app for a tab (LW_EVENT_NEW_WINDOW, number 1) and waits for lw_window_reply, 3 s at
// most; the new tab's page, or null (no window: the page sees a blocked popup).
RefPtr<WebCore::Page> createWindow(lw_view& opener, const URL&, const String& referrer);

// Web thread: asks the app for files (LW_EVENT_FILE_CHOOSER); the chooser waits for lw_file_chooser_reply.
void runFileChooser(lw_view&, WebCore::FileChooser&);

} // namespace Lumia
