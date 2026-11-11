/* POSIX/desktop shims for Navit on Windows Phone 8.1 (declared in wp81_force.h). */
#include "wp81_force.h"
#include <stdio.h>

/* --- snprintf with C99 semantics (VS2013's _snprintf does not terminate on truncation) --- */
int navit_wp81_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    int n;
    va_list copy;
    va_copy(copy, ap);
    n = _vscprintf(fmt, copy);
    va_end(copy);
    if (buf && size) {
        _vsnprintf_s(buf, size, _TRUNCATE, fmt, ap);
    }
    return n;
}

int navit_wp81_snprintf(char *buf, size_t size, const char *fmt, ...) {
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = navit_wp81_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* --- environment: a small table, set by the host before main_real --- */
#define ENV_MAX 64
static char *env_table[ENV_MAX];
static SRWLOCK env_lock = SRWLOCK_INIT;

static int env_find(const char *name, size_t len) {
    int i;
    for (i = 0; i < ENV_MAX; i++)
        if (env_table[i] && !strncmp(env_table[i], name, len) && env_table[i][len] == '=')
            return i;
    return -1;
}

char *navit_wp81_getenv(const char *name) {
    int i;
    char *ret = NULL;
    AcquireSRWLockExclusive(&env_lock);
    i = env_find(name, strlen(name));
    if (i >= 0)
        ret = env_table[i] + strlen(name) + 1;
    ReleaseSRWLockExclusive(&env_lock);
    return ret;
}

int navit_wp81_setenv(const char *name, const char *value, int overwrite) {
    size_t len = strlen(name);
    int i, ret = 0;
    char *entry;
    AcquireSRWLockExclusive(&env_lock);
    i = env_find(name, len);
    if (i >= 0 && !overwrite) {
        ReleaseSRWLockExclusive(&env_lock);
        return 0;
    }
    entry = (char *)malloc(len + strlen(value) + 2);
    if (!entry) {
        ReleaseSRWLockExclusive(&env_lock);
        return -1;
    }
    sprintf(entry, "%s=%s", name, value);
    if (i < 0) {
        for (i = 0; i < ENV_MAX && env_table[i]; i++)
            ;
    }
    if (i < ENV_MAX) {
        /* the old value may still be referenced by a caller of getenv: keep it allocated */
        env_table[i] = entry;
    } else {
        free(entry);
        ret = -1;
    }
    ReleaseSRWLockExclusive(&env_lock);
    return ret;
}

int navit_wp81_putenv(const char *assignment) {
    const char *eq = strchr(assignment, '=');
    char name[128];
    size_t len;
    if (!eq)
        return -1;
    len = eq - assignment;
    if (len >= sizeof(name))
        return -1;
    memcpy(name, assignment, len);
    name[len] = '\0';
    return navit_wp81_setenv(name, eq + 1, 1);
}

/* --- desktop Win32 replacements --- */
DWORD navit_wp81_GetModuleFileNameA(HMODULE module, char *name, DWORD size) {
    const char *prefix = navit_wp81_getenv("NAVIT_PREFIX");
    (void)module;
    if (!prefix || !size)
        return 0;
    _snprintf_s(name, size, _TRUNCATE, "%s\\navit.exe", prefix);
    return (DWORD)strlen(name);
}

int navit_wp81_GetLocaleInfoA(LCID locale, LCTYPE type, char *data, int size) {
    const char *value = NULL;
    (void)locale;
    /* the app is Italian only; LANG is set by the host as well */
    if (type == LOCALE_SABBREVLANGNAME)
        value = "ITA";
    else if (type == LOCALE_SABBREVCTRYNAME)
        value = "ITA";
    if (!value || size <= 0)
        return 0;
    strncpy_s(data, size, value, _TRUNCATE);
    return (int)strlen(data) + 1;
}

/* --- log sink: the host installs a callback (Mappe forwards it to its log and POST /mappe) --- */
void (*navit_wp81_log_hook)(const char *text);

void navit_wp81_log(const char *text) {
    if (navit_wp81_log_hook)
        navit_wp81_log_hook(text);
    else
        OutputDebugStringA(text);
}

/* --- file search: FindFirstFileA/FindNextFileA on top of the app-partition wide APIs --- */
static void find_data_to_ansi(const WIN32_FIND_DATAW *w, WIN32_FIND_DATAA *a) {
    memset(a, 0, sizeof(*a));
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
    WideCharToMultiByte(CP_UTF8, 0, w->cFileName, -1, a->cFileName, sizeof(a->cFileName), NULL, NULL);
}

HANDLE navit_wp81_FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *data) {
    wchar_t wpattern[MAX_PATH];
    WIN32_FIND_DATAW wdata;
    HANDLE h;
    if (!MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, MAX_PATH))
        return INVALID_HANDLE_VALUE;
    h = FindFirstFileExW(wpattern, FindExInfoBasic, &wdata, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE)
        find_data_to_ansi(&wdata, data);
    return h;
}

BOOL navit_wp81_FindNextFileA(HANDLE find, WIN32_FIND_DATAA *data) {
    WIN32_FIND_DATAW wdata;
    if (!FindNextFileW(find, &wdata))
        return FALSE;
    find_data_to_ansi(&wdata, data);
    return TRUE;
}

/* --- support/win32/mmap.c replacement: no memory-mapped files, Navit falls back to read() --- */
void *mmap_readonly_win32(const char *name, long *map_handle_ptr, long *map_file_ptr) {
    (void)name;
    *map_handle_ptr = 0;
    *map_file_ptr = 0;
    return NULL;
}

void mmap_unmap_win32(void *mem_ptr, long map_handle, long map_file) {
    (void)mem_ptr;
    (void)map_handle;
    (void)map_file;
}
