// Navit event loop for Windows Phone 8.1 ("wp81"): runs on the thread that calls navit_wp81_main().
// Same semantics as navit/event_glib.c: one-shot timeouts are freed after they fire, multi timeouts and idles
// stay until removed. Other threads talk to Navit only through navit_wp81_post().
#include <deque>
#include <vector>
#include <algorithm>

extern "C" {
#include "config.h"
#include "item.h"
#include "attr.h"
#include "callback.h"
#include "config_.h"
#include "debug.h"
#include "event.h"
#include "plugin.h"
int main_real(int argc, char *const *argv);
}
#include "navit_wp81.h"

struct event_timeout {
    unsigned long long due;
    int interval;
    int multi;
    struct callback *cb;
    bool removed;
};

struct event_idle {
    int priority;
    struct callback *cb;
    bool removed;
};

namespace {

struct Task {
    navit_wp81_task_fn fn;
    void *data;
};

SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_wake = CONDITION_VARIABLE_INIT;
std::deque<Task> g_tasks;          // guarded by g_lock
bool g_quit;                        // guarded by g_lock

// Navit thread only:
std::vector<event_timeout *> g_timeouts;
std::vector<event_idle *> g_idles;
bool g_ready_called;

unsigned long long NowMs() { return GetTickCount64(); }

void RunTasks() {
    std::deque<Task> tasks;
    AcquireSRWLockExclusive(&g_lock);
    tasks.swap(g_tasks);
    ReleaseSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < tasks.size(); i++)
        tasks[i].fn(tasks[i].data);
}

void RunTimeouts() {
    unsigned long long now = NowMs();
    std::vector<event_timeout *> due;
    for (size_t i = 0; i < g_timeouts.size(); i++)
        if (!g_timeouts[i]->removed && g_timeouts[i]->due <= now)
            due.push_back(g_timeouts[i]);
    for (size_t i = 0; i < due.size(); i++) {
        event_timeout *t = due[i];
        if (t->removed)
            continue;
        if (t->multi)
            t->due = now + (t->interval > 0 ? t->interval : 0);
        else
            t->removed = true;  // freed by Purge, like glib frees it after the call
        callback_call_0(t->cb);
    }
}

void RunIdles() {
    std::vector<event_idle *> idles;
    for (size_t i = 0; i < g_idles.size(); i++)
        if (!g_idles[i]->removed)
            idles.push_back(g_idles[i]);
    std::stable_sort(idles.begin(), idles.end(),
                     [](const event_idle *a, const event_idle *b) { return a->priority < b->priority; });
    for (size_t i = 0; i < idles.size(); i++)
        if (!idles[i]->removed)
            callback_call_0(idles[i]->cb);
}

void Purge() {
    for (size_t i = 0; i < g_timeouts.size();) {
        if (g_timeouts[i]->removed) {
            delete g_timeouts[i];
            g_timeouts.erase(g_timeouts.begin() + i);
        } else
            i++;
    }
    for (size_t i = 0; i < g_idles.size();) {
        if (g_idles[i]->removed) {
            delete g_idles[i];
            g_idles.erase(g_idles.begin() + i);
        } else
            i++;
    }
}

bool HasIdles() {
    for (size_t i = 0; i < g_idles.size(); i++)
        if (!g_idles[i]->removed)
            return true;
    return false;
}

DWORD WaitMs() {
    if (HasIdles())
        return 0;
    unsigned long long now = NowMs(), next = 0;
    bool any = false;
    for (size_t i = 0; i < g_timeouts.size(); i++) {
        if (g_timeouts[i]->removed)
            continue;
        if (!any || g_timeouts[i]->due < next)
            next = g_timeouts[i]->due;
        any = true;
    }
    if (!any)
        return INFINITE;
    return next <= now ? 0 : (DWORD)(next - now);
}

void CallReadyHook() {
    if (g_ready_called || !navit_wp81_ready_hook)
        return;
    struct navit *nav = navit_wp81_navit();
    if (!nav)
        return;
    g_ready_called = true;
    navit_wp81_ready_hook(nav);
}

void MainLoopRun() {
    for (;;) {
        RunTasks();
        CallReadyHook();
        RunTimeouts();
        RunIdles();
        Purge();
        DWORD wait = WaitMs();
        AcquireSRWLockExclusive(&g_lock);
        if (g_quit) {
            ReleaseSRWLockExclusive(&g_lock);
            break;
        }
        if (g_tasks.empty() && wait != 0)
            SleepConditionVariableSRW(&g_wake, &g_lock, wait, 0);
        bool quit = g_quit;
        ReleaseSRWLockExclusive(&g_lock);
        if (quit)
            break;
    }
}

void MainLoopQuit() {
    AcquireSRWLockExclusive(&g_lock);
    g_quit = true;
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_wake);
}

struct event_watch *AddWatch(int fd, enum event_watch_cond cond, struct callback *cb) {
    dbg(lvl_error, "watches are not supported on wp81 (fd %d)", fd);
    return NULL;
}

void RemoveWatch(struct event_watch *ev) {}

struct event_timeout *AddTimeout(int timeout, int multi, struct callback *cb) {
    event_timeout *t = new event_timeout;
    t->interval = timeout;
    t->multi = multi;
    t->cb = cb;
    t->removed = false;
    t->due = NowMs() + (timeout > 0 ? timeout : 0);
    g_timeouts.push_back(t);
    return t;
}

void RemoveTimeout(struct event_timeout *ev) {
    if (ev)
        ev->removed = true;
}

struct event_idle *AddIdle(int priority, struct callback *cb) {
    event_idle *i = new event_idle;
    i->priority = priority;
    i->cb = cb;
    i->removed = false;
    g_idles.push_back(i);
    return i;
}

void RemoveIdle(struct event_idle *ev) {
    if (ev)
        ev->removed = true;
}

void CallCallbackTask(void *data) {
    callback_list_call_0((struct callback_list *)data);
}

void CallCallback(struct callback_list *cb) {
    navit_wp81_post(CallCallbackTask, cb);
}

struct event_methods g_methods = {
    MainLoopRun, MainLoopQuit, AddWatch, RemoveWatch, AddTimeout, RemoveTimeout, AddIdle, RemoveIdle, CallCallback,
};

struct event_priv *EventNew(struct event_methods *meth) {
    *meth = g_methods;
    return (struct event_priv *)&g_methods;
}

}  // namespace

extern "C" {

void (*navit_wp81_ready_hook)(struct navit *nav);

void navit_wp81_post(navit_wp81_task_fn fn, void *data) {
    Task t = {fn, data};
    AcquireSRWLockExclusive(&g_lock);
    g_tasks.push_back(t);
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_wake);
}

void navit_wp81_quit(void) {
    MainLoopQuit();
}

struct navit *navit_wp81_navit(void) {
    struct attr navit;
    if (config && config_get_attr(config, attr_navit, &navit, NULL))
        return navit.u.navit;
    return NULL;
}

int navit_wp81_main(int argc, char **argv) {
    return main_real(argc, argv);
}

void module_event_wp81_init(void) {
    plugin_register_category_event("wp81", EventNew);
}

}  // extern "C"
