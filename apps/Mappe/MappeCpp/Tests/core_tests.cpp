// Tests for the pure core. Build on the PC: g++ -std=c++11 -Wall -I../Core core_tests.cpp ../Core/*.cpp
// (or with MSVC x86 in the VM: cl /EHsc /std:c++14 ...).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Geo.h"
#include "LruCache.h"
#include "TileMath.h"
#include "Viewport.h"

using namespace mappe;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

static void TestTileKey() {
    std::int64_t k = TileKey(19, 524287, 300000);
    int z, x, y;
    DecodeTileKey(k, z, x, y);
    CHECK(z == 19 && x == 524287 && y == 300000);
    CHECK(TileKey(3, 1, 2) != TileKey(3, 2, 1));
    CHECK(TileFileName(5, 17, 11) == "5_17_11.png");
}

static void TestWrapAndValid() {
    CHECK(WrapTileX(-1, 3) == 7);
    CHECK(WrapTileX(8, 3) == 0);
    CHECK(WrapTileX(3, 3) == 3);
    CHECK(IsValidTile(0, 0, 0));
    CHECK(!IsValidTile(1, 2, 0));
    CHECK(!IsValidTile(20, 0, 0));
}

static void TestBuildUrl() {
    CHECK(BuildTileUrl("https://tile.openstreetmap.org/{z}/{x}/{y}.png", 6, 33, 22) ==
          "https://tile.openstreetmap.org/6/33/22.png");
}

static void TestLatLonRoundTrip() {
    // Rome: 41.9 N, 12.5 E
    double nx, ny;
    LatLonToNorm(41.9, 12.5, nx, ny);
    double lat, lon;
    NormToLatLon(nx, ny, lat, lon);
    CHECK(Near(lat, 41.9, 1e-9));
    CHECK(Near(lon, 12.5, 1e-9));
    // Greenwich equator is the centre of the world.
    LatLonToNorm(0.0, 0.0, nx, ny);
    CHECK(Near(nx, 0.5, 1e-12) && Near(ny, 0.5, 1e-12));
}

static void TestFallback() {
    // Only the zoom-4 ancestor is ready for tile (6, 37, 21).
    auto ready = [](int z, int x, int y) { return z == 4 && x == 9 && y == 5; };
    TileFallback f;
    CHECK(FindFallback(6, 37, 21, ready, 3, f));
    CHECK(f.depth == 2 && f.z == 4 && f.x == 9 && f.y == 5);
    CHECK(f.subX == (37 & 3) && f.subY == (21 & 3));
    // Depth limit respected.
    CHECK(!FindFallback(6, 37, 21, ready, 1, f));
    // Nothing ready.
    CHECK(!FindFallback(6, 37, 21, [](int, int, int) { return false; }, 5, f));
}

static void TestLruCache() {
    LruCache<int, int> c(2);
    c.Put(1, 10);
    c.Put(2, 20);
    int v = 0;
    CHECK(c.TryGet(1, v) && v == 10);  // 1 becomes most recent
    c.Put(3, 30);                      // evicts 2
    CHECK(!c.Contains(2));
    CHECK(c.Contains(1) && c.Contains(3));
    CHECK(c.Size() == 2);
    CHECK(c.Remove(1));
    CHECK(!c.TryGet(1, v));
    c.Put(3, 33);
    CHECK(c.TryGet(3, v) && v == 33);
}

static void TestViewportLevelAndScale() {
    Viewport vp;
    vp.Width = 480;
    vp.Height = 800;
    vp.Zoom = 12.4;
    CHECK(vp.Level() == 12);
    CHECK(Near(vp.Scale(), std::pow(2.0, 0.4), 1e-12));
    vp.Zoom = 12.0;
    CHECK(vp.Level() == 12 && Near(vp.Scale(), 1.0, 1e-12));
}

static void TestVisibleTilesCoverViewport() {
    Viewport vp;
    vp.Width = 480;
    vp.Height = 800;
    vp.Zoom = 6.0;
    vp.SetCenterLatLon(41.9, 12.5);
    std::vector<TileCell> cells = vp.VisibleTiles(0);
    // 480x800 at zoom 6 with 256 px tiles: at most 3 columns and 4 rows.
    CHECK(!cells.empty());
    CHECK(cells.size() <= 3 * 4);
    // Every cell has positive size and its key is valid.
    bool ok = true;
    for (const TileCell& c : cells) {
        if (c.width < 1 || c.height < 1 || !IsValidTile(c.z, c.x, c.y)) ok = false;
    }
    CHECK(ok);
    // The tile under the centre is among them.
    int tx, ty;
    LatLonToTile(41.9, 12.5, 6, tx, ty);
    bool found = false;
    for (const TileCell& c : cells) {
        if (c.x == tx && c.y == ty) found = true;
    }
    CHECK(found);
    // With margin 1 we get more tiles, and the list is sorted nearest first.
    std::vector<TileCell> wide = vp.VisibleTiles(1);
    CHECK(wide.size() > cells.size());
    double cx = 240, cy = 400;
    double d0 = std::pow(wide[0].left + wide[0].width / 2.0 - cx, 2) +
                std::pow(wide[0].top + wide[0].height / 2.0 - cy, 2);
    double dn = std::pow(wide.back().left + wide.back().width / 2.0 - cx, 2) +
                std::pow(wide.back().top + wide.back().height / 2.0 - cy, 2);
    CHECK(d0 <= dn);
}

static void TestVisibleTilesWrapAtAntimeridian() {
    Viewport vp;
    vp.Width = 480;
    vp.Height = 800;
    vp.Zoom = 3.0;
    vp.SetCenterLatLon(0.0, 179.9);
    std::vector<TileCell> cells = vp.VisibleTiles(1);
    bool allWrapped = true;
    for (const TileCell& c : cells) {
        if (c.x < 0 || c.x >= 8) allWrapped = false;
    }
    CHECK(allWrapped);
}

static void TestZoomAtKeepsPivot() {
    Viewport vp;
    vp.Width = 480;
    vp.Height = 800;
    vp.Zoom = 10.0;
    vp.SetCenterLatLon(41.9, 12.5);
    double lat0, lon0, lat1, lon1;
    vp.ScreenToLatLon(100, 600, lat0, lon0);
    vp.ZoomAt(100, 600, 13.7);
    vp.ScreenToLatLon(100, 600, lat1, lon1);
    CHECK(Near(lat0, lat1, 1e-9) && Near(lon0, lon1, 1e-9));
    CHECK(Near(vp.Zoom, 13.7, 1e-12));
}

static void TestZoomClampAndPan() {
    Viewport vp;
    vp.Width = 480;
    vp.Height = 800;
    vp.Zoom = 6.0;
    vp.ZoomAt(240, 400, 1.0);
    CHECK(Near(vp.Zoom, Viewport::MinZoom, 1e-12));
    vp.ZoomAt(240, 400, 99.0);
    CHECK(Near(vp.Zoom, Viewport::MaxZoom, 1e-12));
    vp.Zoom = 10.0;
    double cx = vp.CenterX;
    vp.Pan(50, 0);
    CHECK(vp.CenterX < cx);  // dragging right moves the map east, centre goes west
    // Latitude is clamped to the world.
    vp.Pan(0, -1e9);
    CHECK(vp.CenterY >= 0.0 && vp.CenterY <= 1.0);
}

static void TestToScreenCentre() {
    Viewport vp;
    vp.Width = 480;
    vp.Height = 800;
    vp.Zoom = 8.0;
    vp.SetCenterLatLon(41.9, 12.5);
    double sx, sy;
    vp.ToScreen(41.9, 12.5, sx, sy);
    CHECK(Near(sx, 240, 1e-6) && Near(sy, 400, 1e-6));
    CHECK(vp.MetersPerPixel(0.0) > vp.MetersPerPixel(60.0));
}

static void TestFormatting() {
    CHECK(FormatCoord(41.9) == "41.9");
    CHECK(FormatCoord(12.5) == "12.5");
    CHECK(FormatCoord(-0.000001) == "0");
    CHECK(FormatCoord(14.12345678) == "14.12346");
    CHECK(UrlEncode(L"caff\u00e8 via") == "caff%C3%A8%20via");
    std::string u = PhotonSearchUrl(L"Roma", 8, true, 41.9, 12.5);
    CHECK(u == "https://photon.komoot.io/api/?q=Roma&limit=8&lat=41.9&lon=12.5");
    CHECK(PhotonReverseUrl(41.9, 12.5) == "https://photon.komoot.io/reverse?lat=41.9&lon=12.5");
    CHECK(OsmLink(41.9, 12.5, 18, "metrosurf") ==
          "metrosurf://www.openstreetmap.org/?mlat=41.9&mlon=12.5#map=18/41.9/12.5");
    CHECK(ToUtf8(L"\U0001F600") == "\xF0\x9F\x98\x80" || sizeof(wchar_t) == 2);
}

static void TestPlaceText() {
    PlaceInfo p;
    p.street = L"Via Appia";
    p.houseNumber = L"10";
    p.postcode = L"00179";
    p.city = L"Roma";
    p.state = L"Lazio";
    p.country = L"Italia";
    CHECK(PlaceTitle(p) == L"Via Appia 10");
    CHECK(PlaceSubtitle(p) == L"00179 Roma, Lazio, Italia");
    PlaceInfo q;
    q.name = L"Colosseo";
    q.city = L"Roma";
    CHECK(PlaceTitle(q) == L"Colosseo");
    CHECK(PlaceSubtitle(q) == L"Roma");
}

int main() {
    TestTileKey();
    TestWrapAndValid();
    TestBuildUrl();
    TestLatLonRoundTrip();
    TestFallback();
    TestLruCache();
    TestViewportLevelAndScale();
    TestVisibleTilesCoverViewport();
    TestVisibleTilesWrapAtAntimeridian();
    TestZoomAtKeepsPivot();
    TestZoomClampAndPan();
    TestToScreenCentre();
    TestFormatting();
    TestPlaceText();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
