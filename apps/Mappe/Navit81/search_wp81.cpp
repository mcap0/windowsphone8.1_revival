// Offline address search on the Navit thread, like gui/internal/gui_internal_search.c: country Italy, then
// town -> street -> house number. Results go back through a callback (Navit thread).
#include <string>
#include <vector>

extern "C" {
#include "config.h"
#include "item.h"
#include "attr.h"
#include "coord.h"
#include "projection.h"
#include "transform.h"
#include "mapset.h"
#include "navit.h"
#include "search.h"
#include "debug.h"
}
#include "navit_wp81.h"

namespace {

struct search_list *g_sl;
bool g_country_set;

struct SearchTask {
    int level;
    int parent_id;
    std::string text;
    navit_wp81_search_fn done;
    void *ctx;
};

struct search_list *List() {
    if (g_sl)
        return g_sl;
    struct navit *nav = navit_wp81_navit();
    struct attr ms;
    if (!nav || !navit_get_attr(nav, attr_mapset, &ms, NULL))
        return NULL;
    g_sl = search_list_new(ms.u.mapset);
    return g_sl;
}

void SetCountry(struct search_list *sl) {
    if (g_country_set)
        return;
    struct attr a;
    a.type = attr_country_iso2;
    a.u.str = (char *)"IT";
    search_list_search(sl, &a, 0);
    while (search_list_get_result(sl))
        ;
    g_country_set = true;
}

std::string Join(const char *a, const char *b) {
    std::string s = a ? a : "";
    if (b && *b) {
        if (!s.empty())
            s += ", ";
        s += b;
    }
    return s;
}

void SearchOnNavit(void *data) {
    SearchTask *t = (SearchTask *)data;
    std::vector<navit_wp81_search_item> items;
    std::vector<std::string> strings;  // keeps the texts alive until the callback returns
    struct search_list *sl = List();
    if (sl) {
        SetCountry(sl);
        struct attr a;
        a.u.str = (char *)t->text.c_str();
        if (t->level == 1) {
            search_list_select(sl, attr_town_or_district_name, 0, 0);
            a.type = attr_town_or_district_name;
        } else if (t->level == 2) {
            search_list_select(sl, attr_town_or_district_name, 0, 0);
            search_list_select(sl, attr_town_or_district_name, t->parent_id, 1);
            search_list_select(sl, attr_street_name, 0, 0);
            a.type = attr_street_name;
        } else {
            search_list_select(sl, attr_street_name, 0, 0);
            search_list_select(sl, attr_street_name, t->parent_id, 1);
            a.type = attr_house_number;
        }
        search_list_search(sl, &a, 1);
        struct search_list_result *res;
        strings.reserve(200);
        while ((res = search_list_get_result(sl)) && items.size() < 60) {
            navit_wp81_search_item it;
            it.id = res->id;
            it.lat = it.lon = 0;
            if (res->c) {
                struct coord c;
                struct coord_geo g;
                c.x = res->c->x;
                c.y = res->c->y;
                transform_to_geo(res->c->pro, &c, &g);
                it.lat = g.lat;
                it.lon = g.lng;
            }
            std::string title, subtitle;
            if (t->level == 1 && res->town) {
                title = res->town->common.town_name ? res->town->common.town_name : "";
                subtitle = Join(res->town->common.district_name, res->town->common.postal);
            } else if (t->level == 2 && res->street) {
                title = res->street->name ? res->street->name : "";
                subtitle = res->town ? (res->town->common.town_name ? res->town->common.town_name : "") : "";
            } else if (t->level == 3 && res->house_number) {
                title = Join(res->street ? res->street->name : NULL, res->house_number->house_number);
                subtitle = res->town ? (res->town->common.town_name ? res->town->common.town_name : "") : "";
            } else
                continue;
            if (title.empty())
                continue;
            strings.push_back(title);
            strings.push_back(subtitle);
            it.title = NULL;
            it.subtitle = NULL;
            items.push_back(it);
        }
        for (size_t i = 0; i < items.size(); i++) {
            items[i].title = strings[2 * i].c_str();
            items[i].subtitle = strings[2 * i + 1].c_str();
        }
    }
    t->done(t->level, items.empty() ? NULL : &items[0], (int)items.size(), t->ctx);
    delete t;
}

}  // namespace

extern "C" void navit_wp81_search(int level, int parent_id, const char *text, navit_wp81_search_fn done, void *ctx) {
    SearchTask *t = new SearchTask;
    t->level = level;
    t->parent_id = parent_id;
    t->text = text ? text : "";
    t->done = done;
    t->ctx = ctx;
    navit_wp81_post(SearchOnNavit, t);
}
