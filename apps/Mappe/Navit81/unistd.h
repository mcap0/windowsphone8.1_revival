/* Minimal unistd.h for MSVC (navit_lfs.h includes it unconditionally) */
#ifndef NAVIT_WP81_UNISTD_H
#define NAVIT_WP81_UNISTD_H
#include <io.h>
#include <direct.h>
#ifndef F_OK
#define F_OK 0
#define W_OK 2
#define R_OK 4
#endif
#endif
