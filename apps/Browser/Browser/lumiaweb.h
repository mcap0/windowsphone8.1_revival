/*
 * lumiaweb.h — C API of the Lumia WebKit port (webkit.dll). DRAFT for phase 2: names may still change.
 *
 * Plain C, so the app (C++/CX, VS2013 v120_wp81) and the engine (clang-cl, C++23) share no C++ types or allocators.
 *
 * Threading contract (the whole point of the port):
 *   - Every function returns immediately. Work is posted to the engine's threads; nothing ever waits for the Web
 *     thread (the one that runs the page's JavaScript, style and layout).
 *   - Input functions (lw_input_*) may be called from the app's input thread (CoreIndependentInputSource) and go
 *     straight to the scrolling/compositor threads; what reaches the page is queued in the Web thread's input lane,
 *     ahead of network, timers and page tasks.
 *   - Events come back through lw_config.callback on engine threads; strings in an event are valid only during the
 *     callback (copy them). The app forwards to its UI thread when it needs XAML.
 *   - Strings are UTF-16 (wchar_t) like the rest of the app.
 */
#pragma once

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(LW_BUILDING)
#define LW_API __declspec(dllexport)
#else
#define LW_API __declspec(dllimport)
#endif

typedef struct lw_view lw_view; /* one tab: a page, its history, its compositor surface */

/* ---- events: engine -> app ---- */

typedef enum lw_event_type {
    LW_EVENT_TITLE = 1,        /* text: page title */
    LW_EVENT_URL,              /* text: committed URL */
    LW_EVENT_PROGRESS,         /* number: 0-100 */
    LW_EVENT_LOAD_STATE,       /* number: lw_load_state; LW_LOAD_FAILED also has text: error, text2: failing URL */
    LW_EVENT_HISTORY,          /* number: bit 0 = can go back, bit 1 = can go forward; sent when either changes */
    LW_EVENT_FIELD_FOCUS,      /* field: an editable element got focus; field.kind == LW_FIELD_NONE on blur */
    LW_EVENT_FIELD_UPDATE,     /* field: the page changed the focused field (text, selection); field.sequence = last
                                  edit the page had applied when it changed it */
    LW_EVENT_DIALOG,           /* id, number: lw_dialog_kind, text: message, text2: default value. Reply lw_dialog_reply:
                                  the page waits for it (its tab is frozen, the others too: one Web thread). Only the
                                  visible tab of a running app gets dialogs; elsewhere they are dismissed at once. */
    LW_EVENT_FILE_CHOOSER,     /* id, text: accepted types, number: 1 = multiple. Reply lw_file_chooser_reply */
    LW_EVENT_DOWNLOAD,         /* id, text: URL, text2: path of the file being written (in download_directory; empty when
                                  it failed before the file existed), number: size or -1. A response the engine
                                  does not show is saved: the page's own load goes on into the file. Download events
                                  may come with a null view (the tab is gone). lw_download_reply(view, id, NULL)
                                  cancels it */
    LW_EVENT_NEW_WINDOW,       /* text: URL, text2: referrer. number 0: a link with target=_blank, the app opens the
                                  URL in a tab. number 1: window.open; the page waits (3 s at most) for its new
                                  window: the app makes a tab with a SwapChainPanel and calls lw_window_reply(view,
                                  id, panel...), which returns the new tab's view; the engine loads the URL into it
                                  and the opener keeps talking to it (sign-in popups) */
    LW_EVENT_CLOSE_WINDOW,     /* the page called window.close(): the app closes its tab */
    LW_EVENT_CONTEXT_MENU,     /* long press: text: link URL, text2: image URL, field.text: the link's text or the
                                  paragraph under the finger, field rect: its rect in view pixels */
    LW_EVENT_FULLSCREEN,       /* number: 1 = an element of the page went fullscreen (requestFullscreen): the app hides
                                  its bars and the view takes the whole screen; 0 = it ended. The app ends it itself
                                  (Back) with lw_view_exit_fullscreen */
    LW_EVENT_SCRIPT_RESULT,    /* id (from lw_view_run_script), number: 0 ok / 1 exception, text: result as text */
    LW_EVENT_CONSOLE,          /* number: level, text: message, text2: source:line */
    LW_EVENT_TRACE,            /* text: one latency-trace line (see lw_trace_enable) */
    LW_EVENT_FIND_RESULT,      /* id (from lw_view_find), number: matches in the page (0 = none) */
    LW_EVENT_DOWNLOAD_PROGRESS,/* id (from LW_EVENT_DOWNLOAD), number: bytes written so far (at most twice a second) */
    LW_EVENT_DOWNLOAD_FINISHED,/* id, number: 0 done, 1 failed, 2 cancelled; text: path, text2: error. A file that did
                                  not arrive whole is deleted */
    LW_EVENT_PICKER,           /* id, number: lw_picker_kind. A form control whose choice is made in the app:
                                  LW_PICKER_SELECT   text: the <select>'s items, one per line; the line's first character
                                                     is ' ' item, '*' the selected item, '!' disabled item, '#' group
                                                     label, '-' separator. Reply: the chosen line's index as a number.
                                  LW_PICKER_DATE     text: "type\nvalue" (type date, time, datetime-local, month, week;
                                                     value as the input holds it, may be empty), text2:
                                                     "minimum\nmaximum\nstep" as numbers (milliseconds; months for
                                                     month). Reply: the new value in the input's format.
                                  LW_PICKER_COLOR    text: "#rrggbb", text2: the page's suggested colours, one per line.
                                                     Reply: "#rrggbb".
                                  LW_PICKER_CLOSE    the page ended the picker with this id: the app closes it, no reply.
                                  Reply with lw_picker_reply; a NULL value = closed without choosing. */
    LW_EVENT_PASSWORD,         /* id, number: how many passwords the server has already refused for this load (0 = first
                                  question). text: the server that asks, as "scheme://host[:port]"; text2: the realm, the
                                  server's own description (untrusted text, may be empty). HTTP authentication (401).
                                  Reply with lw_password_reply. */
    LW_EVENT_PERMISSION,       /* id, number: lw_permission_kind, text: the site that asks, as "scheme://host[:port]".
                                  Reply with lw_permission_reply; the engine keeps the answer for that site until the
                                  app ends */
    LW_EVENT_GEOLOCATION,      /* number: 1 = a page watches the position: the app starts its position source and
                                  reports with lw_geolocation_update / lw_geolocation_error; 2 = the same, with the
                                  best accuracy the phone has (GPS); 0 = nobody watches any more. One source for every
                                  tab; the view is only the one whose page caused the change */
    LW_EVENT_NOTIFICATION,     /* id, number: 1 = show a notification, text: title, text2: body (a tap on it:
                                  lw_notification_clicked); 0 = the page closed the notification with this id */
    LW_EVENT_SHARE,            /* navigator.share: text: the words to share (may be empty), text2: the address (may be
                                  empty). The app opens the phone's share screen */
    LW_EVENT_CLIPBOARD         /* text: what a page just copied (a "copy" button, document.execCommand, a selection).
                                  It is on the engine's own clipboard, which pages of the app paste from; WP8.1 gives
                                  apps no clipboard, so the app shows the text for the phone's own copy */
} lw_event_type;

typedef enum lw_permission_kind { LW_PERMISSION_GEOLOCATION = 1, LW_PERMISSION_NOTIFICATIONS } lw_permission_kind;

typedef enum lw_picker_kind { LW_PICKER_CLOSE = 0, LW_PICKER_SELECT, LW_PICKER_DATE, LW_PICKER_COLOR } lw_picker_kind;

typedef enum lw_load_state { LW_LOAD_STARTED = 1, LW_LOAD_COMMITTED, LW_LOAD_FINISHED, LW_LOAD_FAILED } lw_load_state;

typedef enum lw_dialog_kind { LW_DIALOG_ALERT = 1, LW_DIALOG_CONFIRM, LW_DIALOG_PROMPT, LW_DIALOG_BEFOREUNLOAD } lw_dialog_kind;

typedef enum lw_field_kind {
    LW_FIELD_NONE = 0, LW_FIELD_TEXT, LW_FIELD_URL, LW_FIELD_EMAIL, LW_FIELD_NUMBER, LW_FIELD_PHONE, LW_FIELD_PASSWORD,
    LW_FIELD_SEARCH, LW_FIELD_MULTILINE /* textarea, contenteditable */
} lw_field_kind;

typedef struct lw_field {
    int kind;                  /* lw_field_kind */
    const wchar_t* text;       /* value; for contenteditable the text of the block holding the caret */
    int selection_start;       /* in UTF-16 units within text */
    int selection_end;
    int x, y, width, height;   /* field rect in view pixels */
    int enter_action;          /* 0 = newline/default, 1 = go, 2 = send, 3 = search, 4 = next, 5 = done (enterkeyhint) */
    int max_length;            /* -1 = none */
    uint32_t sequence;         /* LW_EVENT_FIELD_UPDATE: last lw_text_edit sequence applied before the page's change */
} lw_field;

typedef struct lw_event {
    int type;                  /* lw_event_type */
    int id;
    int64_t number;
    const wchar_t* text;
    const wchar_t* text2;
    const lw_field* field;
} lw_event;

typedef void (*lw_event_callback)(void* context, lw_view* view, const lw_event* event);

/* ---- engine ---- */

typedef struct lw_config {
    const wchar_t* resource_directory;  /* package folder: icudt77l.dat, cacert.pem, content blocker rules */
    const wchar_t* data_directory;      /* persistent: cookies, localStorage, IndexedDB */
    const wchar_t* cache_directory;     /* disposable: HTTP disk cache, bytecode cache */
    uint64_t memory_limit;              /* bytes the OS lets the app use (MemoryManager.AppMemoryUsageLimit) */
    const wchar_t* user_agent;          /* default User-Agent; per-view override with lw_view_set_user_agent */
    lw_event_callback callback;
    void* context;
    const wchar_t* download_directory;  /* where downloads are saved (NULL: data_directory\\Downloads) */
    int keep_session_cookies;           /* 1: cookies without an expiry survive a restart of the app (WP8.1 ends apps in
                                           the background; to the user that is not "closing the browser") */
} lw_config;

LW_API int lw_init(const lw_config* config);         /* 0 = ok; starts the Web, network and compositor threads */
/* App suspending: the pages become hidden (visibilitychange; rAF, animations and media stop), rendering pauses,
 * localStorage is written to disk, caches and JIT code are released and the GPU driver's spare memory is trimmed.
 * `done(context)` (may be NULL) comes on an engine thread when all of that is finished: the app keeps its
 * SuspendingDeferral until then, and completes it itself shortly before the deadline in any case. */
LW_API void lw_suspend(void (*done)(void* context), void* context);
LW_API void lw_resume(void);                         /* pages visible again, rendering resumes */
LW_API void lw_memory_pressure(int critical);        /* OS memory warning */
/* The phone's connection changed (NetworkInformation.NetworkStatusChanged). online: there is a way to the internet
   (navigator.onLine, online/offline events). switched: it now goes through another network (Wi-Fi <-> cellular):
   connections opened on the old one are dead, so none is reused and open WebSockets fail (the page reconnects). */
LW_API void lw_network_changed(int online, int switched);
LW_API void lw_memory_report(void);                  /* development: where the memory is, as LW_EVENT_TRACE lines */
LW_API void lw_clear_visited_links(void);            /* the app cleared its history: no link shows as visited */
/* Everything the engine keeps for the sites, of the kinds in `what`, for every site (the private tabs' things go by
   themselves with the last private tab). */
typedef enum lw_clear_kind {
    LW_CLEAR_COOKIES = 1,      /* cookies (logins) */
    LW_CLEAR_CACHE = 2,        /* disk and memory caches, compiled scripts */
    LW_CLEAR_STORAGE = 4,      /* localStorage, IndexedDB */
    LW_CLEAR_PASSWORDS = 8,    /* HTTP passwords given in this run */
    LW_CLEAR_PERMISSIONS = 16, /* the answers to LW_EVENT_PERMISSION given in this run */
    LW_CLEAR_HISTORY = 32      /* visited links */
} lw_clear_kind;
LW_API void lw_clear_data(unsigned what);
/* The app's settings. LW_OPTION_BLOCKER: the ad and tracker blocker, 1 on (the default), 0 off. */
typedef enum lw_option { LW_OPTION_BLOCKER = 1 } lw_option;
LW_API void lw_set_option(int option, int value);
LW_API void lw_trace_enable(int enabled);            /* latency trace lines as LW_EVENT_TRACE */
LW_API void lw_gpu_test(void);                      /* record a 6 s frame timeline to the log (tools/timeline.py) */
LW_API void lw_profile(int milliseconds);            /* sample the Web thread (<= 30 s, 0 = 10 s), profile to the log */
/* development: a command typed in the address box, UTF-8; the answer is a log line. "bytecode": the bytecode cache
   off or on again for the scripts run from now on (to measure what it gives on a page). */
LW_API void lw_command(const char* command);

/* ---- views (tabs) ---- */

/* swap_chain_panel: the XAML SwapChainPanel as IInspectable*; the compositor thread draws into it. */
LW_API lw_view* lw_view_create(void* swap_chain_panel, int width, int height, float device_scale);
/* A private tab: cookies, passwords, localStorage and IndexedDB of its own, in memory only, shared with the other
   private tabs open at the time and gone with the last of them; nothing of it reaches the disk cache, the compiled
   scripts' cache or the visited links. Its windows (window.open, target=_blank through lw_window_reply) are private. */
LW_API lw_view* lw_view_create_private(void* swap_chain_panel, int width, int height, float device_scale);
LW_API void lw_view_destroy(lw_view* view);
LW_API void lw_view_resize(lw_view* view, int width, int height);
LW_API void lw_view_set_visible(lw_view* view, int visible); /* hidden tabs are frozen */
LW_API void lw_view_set_user_agent(lw_view* view, const wchar_t* user_agent);
/* User-Agent for one site, in every tab: `host` and its subdomains (example.com covers www.example.com); a null
   user_agent removes it. navigator.platform follows (iPhone, MacIntel) when the agent names one of them. */
LW_API void lw_set_site_user_agent(const wchar_t* host, const wchar_t* user_agent);

/* Leaving the page (load, reload, back/forward to another document, clearing the site's data) interrupts the script
   it may be running: the next page starts at once instead of after the old page's task (seconds on YouTube). */
LW_API void lw_view_load_url(lw_view* view, const wchar_t* url);
LW_API void lw_view_reload(lw_view* view);
LW_API void lw_view_stop(lw_view* view);
LW_API void lw_view_clear_site_data(lw_view* view); /* cookies, storage, cache of the current site; then reload */
LW_API void lw_view_go_back(lw_view* view);         /* the tab's own history (LW_EVENT_HISTORY), scroll and forms */
LW_API void lw_view_go_forward(lw_view* view);
/* Find in page: highlights every match of `text` (any case) and selects the next one after the current selection (the
   previous one with backwards = 1), scrolling to it; empty text clears the highlights. Result: LW_EVENT_FIND_RESULT. */
LW_API void lw_view_find(lw_view* view, int id, const wchar_t* text, int backwards);
LW_API void lw_view_run_script(lw_view* view, int id, const wchar_t* script); /* result: LW_EVENT_SCRIPT_RESULT */
/* Saves `url` (an image or a link of the page, relative to it) as a file, with the page's cookies: LW_EVENT_DOWNLOAD. */
LW_API void lw_view_download(lw_view* view, const wchar_t* url);
LW_API void lw_view_exit_fullscreen(lw_view* view); /* ends the page's fullscreen (LW_EVENT_FULLSCREEN 0 follows) */

/* ---- input (any thread; normally the app's input thread) ---- */

typedef enum lw_phase { LW_PHASE_BEGIN = 1, LW_PHASE_UPDATE, LW_PHASE_END, LW_PHASE_CANCEL } lw_phase;

/* Raw touch points: every finger's press, moves and release (pointer_id = PointerPoint.PointerId), before the pan,
   zoom or tap calls of the same pointer event. The page gets them as touch and pointer events; the first finger of a
   gesture also starts the tap highlight and tells the engine what the page lets the gesture do. */
LW_API void lw_input_touch(lw_view* view, int pointer_id, int phase, float x, float y, uint64_t timestamp_us);
/* One-finger pan, as the finger moves: (x, y) where it is and (dx, dy) how far it moved since the last call, in view
   pixels; timestamp_us = the pointer's own time (PointerPoint.Timestamp), from which the engine computes the fling
   velocity at LW_PHASE_END and animates the inertia itself, in step with the display. Applied by the scrolling
   thread without the Web thread. Where the page has a non-passive touch listener under the finger the scroll starts
   when the page answers (100 ms at most) and catches up with the finger; a page that cancels the touch, or
   touch-action, keeps the gesture. The caller never waits. */
LW_API void lw_input_pan(lw_view* view, int phase, float x, float y, float dx, float dy, uint64_t timestamp_us);
/* Two-finger pinch: scale since LW_PHASE_BEGIN and the fingers' center. Ignored where the page keeps the gesture. */
LW_API void lw_input_zoom(lw_view* view, int phase, float scale, float center_x, float center_y);
LW_API void lw_input_tap(lw_view* view, float x, float y, uint64_t timestamp_us);       /* highlight at once */
LW_API void lw_input_double_tap(lw_view* view, float x, float y);
LW_API void lw_input_long_press(lw_view* view, float x, float y);
LW_API void lw_input_key(lw_view* view, int virtual_key, int down);

/* ---- text: the native bar above the keyboard ---- */

/* Replace [start, end) of the focused field's text with `text` and set the selection; `sequence` increases by one
   per edit. The engine merges edits that queue up while the page is busy and reports what it applied with
   LW_EVENT_FIELD_UPDATE. */
LW_API void lw_text_edit(lw_view* view, uint32_t sequence, int start, int end, const wchar_t* text,
                         int selection_start, int selection_end);
LW_API void lw_text_enter(lw_view* view); /* Enter/send key of the bar */
LW_API void lw_text_blur(lw_view* view);  /* the bar closed */

/* ---- replies to events ---- */

LW_API void lw_dialog_reply(lw_view* view, int id, int accepted, const wchar_t* value);
LW_API void lw_file_chooser_reply(lw_view* view, int id, const wchar_t* const* paths, int count); /* count 0 = cancel */
LW_API void lw_download_reply(lw_view* view, int id, const wchar_t* path);  /* null = cancel the download */
LW_API void lw_picker_reply(lw_view* view, int id, const wchar_t* value);   /* LW_EVENT_PICKER; null = no choice */
/* LW_EVENT_PASSWORD. user NULL = cancelled: the page shows the server's own "unauthorized" answer. The password is kept
   until the app ends and sent to that server (and realm) without asking again. */
LW_API void lw_password_reply(lw_view* view, int id, const wchar_t* user, const wchar_t* password);
/* LW_EVENT_PERMISSION. allowed: 1 yes, 0 no (both kept for the site until the app ends), -1 the question went away
   unanswered (the site is refused this time and may ask again). */
LW_API void lw_permission_reply(lw_view* view, int id, int allowed);
/* The phone's position while LW_EVENT_GEOLOCATION asked for it: degrees, accuracy in metres; altitude (m), its
   accuracy (m), heading (degrees from north) and speed (m/s) are NaN when unknown. */
LW_API void lw_geolocation_update(double latitude, double longitude, double accuracy, double altitude,
                                  double altitude_accuracy, double heading, double speed);
LW_API void lw_geolocation_error(int code);  /* 1 = the phone's location is off or refused to the app, 2 = no position */
LW_API void lw_notification_clicked(lw_view* view, int id);                  /* the user tapped LW_EVENT_NOTIFICATION id */
/* LW_EVENT_NEW_WINDOW with number 1: the new window's panel and size (as lw_view_create); returns its view, or NULL if
   the page has stopped waiting (then the app may open the URL as an ordinary tab). panel NULL refuses the window. */
LW_API lw_view* lw_window_reply(lw_view* opener, int id, void* swap_chain_panel, int width, int height, float device_scale);

#ifdef __cplusplus
}
#endif
