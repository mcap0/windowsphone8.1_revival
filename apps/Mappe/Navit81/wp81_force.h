/* Forced include (/FI) for every Navit C/C++ file in the Windows Phone 8.1 build.
 * Fills the gaps between what upstream Navit expects (POSIX, C99, desktop Win32) and what VS2013 + the
 * Windows Phone 8.1 app partition provide. Keep upstream sources untouched where a shim here is enough. */
#ifndef NAVIT_WP81_FORCE_H
#define NAVIT_WP81_FORCE_H

#define _CRT_SECURE_NO_WARNINGS 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#define _USE_MATH_DEFINES 1
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1

/* Navit is written against the ANSI Win32 API: keep TCHAR = char everywhere in this lib */
#undef UNICODE
#undef _UNICODE
#include <windows.h>
/* no stdio.h/fcntl.h/io.h here: navit_lfs.h refuses to come after them */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <direct.h>
#include <io.h>
#include <malloc.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

/* minwindef.h defines near/far as empty macros; Navit uses them as parameter names */
#undef near
#undef far
#undef FAR
#define FAR
#undef NEAR
#define NEAR

#ifdef __cplusplus
extern "C" {
#endif

/* C99 keywords and helpers that VS2013's C compiler lacks */
#ifndef __cplusplus
/* Navit's "inline" C functions are also its exported API (C99 rules: a declaration without inline in the header
 * makes them external). MSVC's __inline does not follow that, so in C they become plain functions. */
#define inline
/* and keep support/glib (gutils.h) from turning it back into MSVC's __inline */
#define G_HAVE_INLINE 1
#endif
#ifndef __func__
#define __func__ __FUNCTION__
#endif
/* VS2013 has vsnprintf but no snprintf */
#define snprintf navit_wp81_snprintf
int navit_wp81_snprintf(char *buf, size_t size, const char *fmt, ...);
int navit_wp81_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif

/* POSIX names */
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define O_LARGEFILE 0
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* Environment: the app CRT has no getenv/putenv, Navit uses them for its paths. */
#define getenv navit_wp81_getenv
#define setenv navit_wp81_setenv
#define putenv navit_wp81_putenv
char *navit_wp81_getenv(const char *name);
int navit_wp81_setenv(const char *name, const char *value, int overwrite);
int navit_wp81_putenv(const char *assignment);

/* Desktop-only Win32 used by Navit's Windows code paths */
#define GetTickCount() ((DWORD)GetTickCount64())
DWORD navit_wp81_GetModuleFileNameA(HMODULE module, char *name, DWORD size);
#define GetModuleFileName navit_wp81_GetModuleFileNameA
#define GetModuleFileNameA navit_wp81_GetModuleFileNameA
#define GetModuleFileNameW(module, name, size) (0)
int navit_wp81_GetLocaleInfoA(LCID locale, LCTYPE type, char *data, int size);
#define GetLocaleInfo navit_wp81_GetLocaleInfoA
#define GetLocaleInfoA navit_wp81_GetLocaleInfoA

/* Process spawning does not exist for apps; Navit only uses it for speech/cmdline (not built). */
typedef struct navit_wp81_process_information {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} NAVIT_WP81_PROCESS_INFORMATION;
#define PROCESS_INFORMATION NAVIT_WP81_PROCESS_INFORMATION
#define GetExitCodeProcess(h, code) (*(code) = 0, FALSE)
typedef struct navit_wp81_startupinfo {
    DWORD cb;
} NAVIT_WP81_STARTUPINFO;
#define STARTUPINFO NAVIT_WP81_STARTUPINFO
#define CreateProcess(app, cmd, pa, ta, inherit, flags, env, dir, si, pi) (FALSE)
#ifndef STILL_ACTIVE
#define STILL_ACTIVE 259
#endif

/* Desktop-only synchronisation and file search APIs, mapped to the app-partition ones */
#define InitializeCriticalSection(cs) InitializeCriticalSectionEx((cs), 0, 0)
#define WaitForSingleObject(h, ms) WaitForSingleObjectEx((h), (ms), FALSE)
#define GetSystemInfo GetNativeSystemInfo
HANDLE navit_wp81_FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *data);
BOOL navit_wp81_FindNextFileA(HANDLE find, WIN32_FIND_DATAA *data);
#define FindFirstFileA navit_wp81_FindFirstFileA
#define FindNextFileA navit_wp81_FindNextFileA
#undef FindFirstFile
#undef FindNextFile
#define FindFirstFile navit_wp81_FindFirstFileA
#define FindNextFile navit_wp81_FindNextFileA

/* POSIX getline (bookmarks.c), implemented by Navit's util.c; FILE is struct _iobuf in the MSVC CRT */
struct _iobuf;
ssize_t getline(char **lineptr, size_t *n, struct _iobuf *stream);

/* glib internals used before their headers */
void _g_slice_thread_init_nomessage(void);

/* Debug output goes to the app log (see wp81_compat.c) */
void navit_wp81_log(const char *text);

#ifdef __cplusplus
}
#endif

#endif
