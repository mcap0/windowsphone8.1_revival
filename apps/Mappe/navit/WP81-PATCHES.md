# Navit 0.5.7 on Windows Phone 8.1: changes to upstream sources

Upstream: github.com/navit-gps/navit tag v0.5.7 (pristine copy in `ref/navit`). Only the files below differ;
everything else (shims, config, plugins) lives in `apps/Mappe/Navit81/`. Check with
`diff -ru ref/navit/navit apps/Mappe/navit/navit`.

| File | Change | Why |
|---|---|---|
| navit/debug.c | `#ifdef HAVE_API_WP81` → `navit_wp81_log()` instead of `fprintf(stderr)` | no console in an app |
| navit/speech.c | `NAVIT_OBJECT;` → `NAVIT_OBJECT` | the macro already ends with `;`, MSVC C rejects the empty member |
| navit/speech.c | `ATTR_INT(active, 1)` compound literal → plain initializer `{attr_active, {.num = 1}}` | a compound literal is not a constant initializer for MSVC |
| navit/main.c | `#if defined(HAVE_API_WP81)` before the WIN32/WIN32_CE environment chain | the app sets NAVIT_* itself |
| navit/support/glib/gslice.c | `HAVE_API_WP81`: always_malloc on, page size 4096, no VirtualAlloc | VirtualAlloc is not available to WP8.1 apps |
