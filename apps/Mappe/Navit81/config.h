/* Navit config for the Windows Phone 8.1 ARM build (hand written from config.h.in; see WP81-PATCHES.md). */
#ifndef NAVIT_WP81_CONFIG_H
#define NAVIT_WP81_CONFIG_H

#define HAVE_API_WP81 1          /* our port: WinRT app container, no desktop Win32 */
#define HAVE_API_WIN32_BASE 1    /* Windows-family code paths of Navit */
#define USE_NATIVE_LANGUAGE_SUPPORT 1
#define HAVE_MALLOC_H 1
#define HAVE_STDINT_H 1
#define CACHE_SIZE 1048576

#define GIT_VERSION "v0.5.7"
#define NAVIT_VARIANT "-wp81"
#define PACKAGE_VERSION "0.5.7"
#define PACKAGE_NAME "navit"
#define PACKAGE "navit"
#define LOCALEDIR "locale"
#define NAVIT_VERSION PACKAGE_VERSION "+git:" GIT_VERSION NAVIT_VARIANT

#define HAVE_ZLIB 1
#define USE_ROUTING 1
#define HAVE_STRING_H 1
#define EZXML_NOMMAP 1
#define HAVE_PRAGMA_PACK 1
#define HAVE__ATOI64 1

/* install layout (CMake passes these with -D) */
#define LIB_DIR "lib"
#define SHARE_DIR "share"
#define LOCALE_DIR "locale"
#define IMAGE_DIR "icons"
#define TEXTURE_DIR "textures"
#define HOMECONFIG_DIR ".navit"

#endif
