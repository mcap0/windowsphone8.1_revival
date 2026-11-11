// Commands from the app to Navit (zoom, center, destination, frame capture). Each one posts a task to the Navit
// thread, so the XAML side never touches Navit objects directly.
#include <string>

extern "C" {
#include "config.h"
#include "item.h"
#include "attr.h"
#include "coord.h"
#include "point.h"
#include "projection.h"
#include "transform.h"
#include "navit.h"
#include "debug.h"
}
#include "navit_wp81.h"

namespace {

struct GeoTask {
    double lat, lon;
    std::string name;
    int kind;  // 0 center, 1 destination, 2 position
};

struct pcoord ToPcoord(double lat, double lon) {
    struct coord_geo g;
    struct coord c;
    g.lat = (navit_float)lat;
    g.lng = (navit_float)lon;
    transform_from_geo(projection_mg, &g, &c);
    struct pcoord pc;
    pc.pro = projection_mg;
    pc.x = c.x;
    pc.y = c.y;
    return pc;
}

void GeoOnNavit(void *data) {
    GeoTask *t = (GeoTask *)data;
    struct navit *nav = navit_wp81_navit();
    if (nav) {
        struct pcoord pc = ToPcoord(t->lat, t->lon);
        if (t->kind == 0)
            navit_set_center(nav, &pc, 1);
        else if (t->kind == 1)
            navit_set_destination(nav, &pc, t->name.empty() ? "Destinazione" : t->name.c_str(), 1);
        else
            navit_set_position(nav, &pc);
    }
    delete t;
}

void PostGeo(int kind, double lat, double lon, const char *name) {
    GeoTask *t = new GeoTask;
    t->kind = kind;
    t->lat = lat;
    t->lon = lon;
    if (name)
        t->name = name;
    navit_wp81_post(GeoOnNavit, t);
}

void ZoomOnNavit(void *data) {
    int in = (int)(intptr_t)data;
    struct navit *nav = navit_wp81_navit();
    if (!nav)
        return;
    if (in)
        navit_zoom_in(nav, 2, NULL);
    else
        navit_zoom_out(nav, 2, NULL);
}

void FollowOnNavit(void *data) {
    struct navit *nav = navit_wp81_navit();
    if (nav)
        navit_set_center_cursor(nav, 1, 0);
}

void StopRouteOnNavit(void *data) {
    struct navit *nav = navit_wp81_navit();
    if (nav)
        navit_set_destination(nav, NULL, NULL, 0);
}

struct CaptureTask {
    navit_wp81_capture_fn done;
    void *ctx;
};

void CaptureOnNavit(void *data) {
    CaptureTask *t = (CaptureTask *)data;
    int w = 0, h = 0;
    unsigned char *bgra = navit_wp81_gfx_capture(&w, &h);
    t->done(bgra, w, h, t->ctx);  // the callee owns bgra (free())
    delete t;
}

}  // namespace

extern "C" {

void navit_wp81_zoom(int in) {
    navit_wp81_post(ZoomOnNavit, (void *)(intptr_t)(in ? 1 : 0));
}

void navit_wp81_center(double lat, double lon) {
    PostGeo(0, lat, lon, NULL);
}

void navit_wp81_destination(double lat, double lon, const char *name) {
    PostGeo(1, lat, lon, name);
}

void navit_wp81_set_position(double lat, double lon) {
    PostGeo(2, lat, lon, NULL);
}

void navit_wp81_follow_vehicle(void) {
    navit_wp81_post(FollowOnNavit, NULL);
}

void navit_wp81_stop_route(void) {
    navit_wp81_post(StopRouteOnNavit, NULL);
}

void navit_wp81_capture(navit_wp81_capture_fn done, void *ctx) {
    CaptureTask *t = new CaptureTask;
    t->done = done;
    t->ctx = ctx;
    navit_wp81_post(CaptureOnNavit, t);
}

}  // extern "C"
