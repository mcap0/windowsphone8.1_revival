/* Navit vehicle plugin "wp81": positions come from the app (Windows.Devices.Geolocation.Geolocator) through
 * navit_wp81_vehicle_position(), posted to the Navit thread. Same attributes as vehicle/android. */
#include <config.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <math.h>
#include <time.h>
#include "debug.h"
#include "callback.h"
#include "plugin.h"
#include "coord.h"
#include "item.h"
#include "attr.h"
#include "vehicle.h"
#include "navit_wp81.h"

struct vehicle_priv {
    struct callback_list *cbl;
    struct coord_geo geo;
    double speed;
    double direction;
    double height;
    double radius;
    int fix_type;
    char fixiso8601[64];
    int sats;
    int sats_used;
    int valid;
};

struct position_task {
    double lat, lon, speed, direction, height, radius;
    long long unix_ms;
};

static struct vehicle_priv *the_vehicle;

static void vehicle_wp81_destroy(struct vehicle_priv *priv) {
    if (the_vehicle == priv)
        the_vehicle = NULL;
    g_free(priv);
}

static int vehicle_wp81_position_attr_get(struct vehicle_priv *priv, enum attr_type type, struct attr *attr) {
    switch (type) {
    case attr_position_fix_type:
        attr->u.num = priv->fix_type;
        break;
    case attr_position_height:
        attr->u.numd = &priv->height;
        break;
    case attr_position_speed:
        attr->u.numd = &priv->speed;
        break;
    case attr_position_direction:
        attr->u.numd = &priv->direction;
        break;
    case attr_position_radius:
        attr->u.numd = &priv->radius;
        break;
    case attr_position_qual:
        attr->u.num = priv->sats;
        break;
    case attr_position_sats_used:
        attr->u.num = priv->sats_used;
        break;
    case attr_position_coord_geo:
        attr->u.coord_geo = &priv->geo;
        if (priv->valid == attr_position_valid_invalid)
            return 0;
        break;
    case attr_position_time_iso8601:
        attr->u.str = priv->fixiso8601;
        break;
    case attr_position_valid:
        attr->u.num = priv->valid;
        break;
    default:
        return 0;
    }
    attr->type = type;
    return 1;
}

static struct vehicle_methods vehicle_wp81_methods = {
    vehicle_wp81_destroy,
    vehicle_wp81_position_attr_get,
    NULL,
};

static void position_on_navit(void *data) {
    struct position_task *t = data;
    struct vehicle_priv *v = the_vehicle;
    if (v) {
        time_t secs = (time_t)(t->unix_ms / 1000);
        struct tm *tm = gmtime(&secs);
        v->geo.lat = t->lat;
        v->geo.lng = t->lon;
        if (t->speed >= 0)
            v->speed = t->speed;
        if (t->direction >= 0)
            v->direction = t->direction;
        v->height = t->height;
        v->radius = t->radius;
        if (tm)
            strftime(v->fixiso8601, sizeof(v->fixiso8601), "%Y-%m-%dT%H:%M:%SZ", tm);
        if (!v->fix_type) {
            v->fix_type = 1;
            callback_list_call_attr_0(v->cbl, attr_position_fix_type);
        }
        if (v->valid != attr_position_valid_valid) {
            v->valid = attr_position_valid_valid;
            callback_list_call_attr_0(v->cbl, attr_position_valid);
        }
        callback_list_call_attr_0(v->cbl, attr_position_coord_geo);
    }
    g_free(t);
}

static void fix_on_navit(void *data) {
    int fix = (int)(long)data;
    struct vehicle_priv *v = the_vehicle;
    if (!v || v->fix_type == fix)
        return;
    v->fix_type = fix;
    callback_list_call_attr_0(v->cbl, attr_position_fix_type);
    if (!fix && v->valid == attr_position_valid_valid) {
        v->valid = attr_position_valid_extrapolated_time;
        callback_list_call_attr_0(v->cbl, attr_position_valid);
    }
}

void navit_wp81_vehicle_position(double lat, double lon, double speed, double direction, double height,
                                 double radius, long long unix_ms) {
    struct position_task *t = g_new0(struct position_task, 1);
    t->lat = lat;
    t->lon = lon;
    t->speed = speed;
    t->direction = direction;
    t->height = height;
    t->radius = radius;
    t->unix_ms = unix_ms;
    navit_wp81_post(position_on_navit, t);
}

void navit_wp81_vehicle_fix(int fix) {
    navit_wp81_post(fix_on_navit, (void *)(long)fix);
}

static struct vehicle_priv *vehicle_wp81_new(struct vehicle_methods *meth, struct callback_list *cbl,
        struct attr **attrs) {
    struct vehicle_priv *ret = g_new0(struct vehicle_priv, 1);
    ret->cbl = cbl;
    ret->valid = attr_position_valid_invalid;
    *meth = vehicle_wp81_methods;
    the_vehicle = ret;
    return ret;
}

void module_vehicle_wp81_init(void) {
    plugin_register_category_vehicle("wp81", vehicle_wp81_new);
}
