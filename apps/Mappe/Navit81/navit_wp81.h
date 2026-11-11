/* Host API between the Mappe app (C++/CX, XAML thread) and Navit running on its own thread.
 * Everything marked "posts" is thread-safe and runs later on the Navit thread; hooks are called on the Navit thread. */
#ifndef NAVIT_WP81_H
#define NAVIT_WP81_H

#ifdef __cplusplus
extern "C" {
#endif

struct navit;

typedef void (*navit_wp81_task_fn)(void *data);

/* Runs fn(data) on the Navit thread (posts). */
void navit_wp81_post(navit_wp81_task_fn fn, void *data);
/* Runs Navit (main_real) on the calling thread until navit_wp81_quit(). argv[1] = path of navit.xml. */
int navit_wp81_main(int argc, char **argv);
/* Asks the Navit loop to stop (posts). */
void navit_wp81_quit(void);
/* The navit object from navit.xml, or NULL before the config is loaded (Navit thread only). */
struct navit *navit_wp81_navit(void);

/* Environment for Navit (NAVIT_SHAREDIR, NAVIT_USER_DATADIR, LANG...): set before navit_wp81_main. */
int navit_wp81_setenv(const char *name, const char *value, int overwrite);

/* Drawing surface owned by the app. d2d_context = ID2D1DeviceContext*, swap_chain = IDXGISwapChain1*,
 * dwrite = IDWriteFactory*, wic = IWICImagingFactory*. Call before navit_wp81_main (AddRef'd here). */
void navit_wp81_gfx_init(void *d2d_context, void *swap_chain, void *dwrite, void *wic, int width, int height,
                         float dpi);
/* The SwapChainPanel changed size: resizes the swap chain and tells Navit (posts). */
void navit_wp81_gfx_resize(int width, int height);
/* Touch, in physical pixels of the panel (posts). button 1 = finger. */
void navit_wp81_input_button(int pressed, int button, int x, int y);
void navit_wp81_input_motion(int x, int y);
/* Copy of the last map frame as BGRA (premultiplied), malloc'd; NULL if none (Navit thread only). */
unsigned char *navit_wp81_gfx_capture(int *width, int *height);

/* Position from Windows.Devices.Geolocation (posts). speed km/h, direction degrees, radius = accuracy m. */
void navit_wp81_vehicle_position(double lat, double lon, double speed, double direction, double height,
                                 double radius, long long unix_ms);
/* fix = 0 when the GPS lost the position (posts). */
void navit_wp81_vehicle_fix(int fix);

/* Commands (posts). Coordinates are WGS84 degrees. */
void navit_wp81_zoom(int in);
void navit_wp81_center(double lat, double lon);
void navit_wp81_destination(double lat, double lon, const char *name);
void navit_wp81_set_position(double lat, double lon);
void navit_wp81_follow_vehicle(void);
void navit_wp81_stop_route(void);
/* Frame capture: done(bgra, w, h, ctx) runs on the Navit thread; bgra is malloc'd (free it) or NULL. */
typedef void (*navit_wp81_capture_fn)(unsigned char *bgra, int width, int height, void *ctx);
void navit_wp81_capture(navit_wp81_capture_fn done, void *ctx);

/* Offline address search (posts): level 1 = town in Italy, 2 = street in town parent_id, 3 = house number in street
 * parent_id. done(level, items, count, ctx) runs on the Navit thread; the items live only during the call. */
typedef struct navit_wp81_search_item {
    int id;
    double lat, lon;
    const char *title;
    const char *subtitle;
} navit_wp81_search_item;
typedef void (*navit_wp81_search_fn)(int level, const navit_wp81_search_item *items, int count, void *ctx);
void navit_wp81_search(int level, int parent_id, const char *text, navit_wp81_search_fn done, void *ctx);

/* Hooks set by the app before navit_wp81_main. Called on the Navit thread. */
extern void (*navit_wp81_log_hook)(const char *text);
extern void (*navit_wp81_speech_hook)(const char *text);
extern void (*navit_wp81_keep_screen_on_hook)(void);
extern void (*navit_wp81_ready_hook)(struct navit *nav);

#ifdef __cplusplus
}
#endif

#endif
