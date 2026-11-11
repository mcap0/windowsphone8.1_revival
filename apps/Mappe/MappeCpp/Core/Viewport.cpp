#include "Viewport.h"

#include <algorithm>
#include <cmath>

#include "TileMath.h"

namespace mappe {

const double Viewport::MinZoom = 3.0;
const double Viewport::MaxZoom = 19.0;

double Viewport::ClampZoom(double z) const {
    if (z < MinZoom) return MinZoom;
    if (z > MaxZoom) return MaxZoom;
    return z;
}

int Viewport::Level() const {
    int l = static_cast<int>(std::floor(Zoom));
    if (l < 0) l = 0;
    if (l > MaxTileZoom) l = MaxTileZoom;
    return l;
}

double Viewport::Scale() const {
    return std::pow(2.0, Zoom - Level());
}

double Viewport::WorldSize() const {
    return TileSize * std::pow(2.0, Zoom);
}

void Viewport::SetCenterLatLon(double lat, double lon) {
    LatLonToNorm(lat, lon, CenterX, CenterY);
    ClampCenter();
}

void Viewport::CenterLatLon(double& lat, double& lon) const {
    NormToLatLon(CenterX, CenterY, lat, lon);
}

void Viewport::ToScreen(double lat, double lon, double& sx, double& sy) const {
    double nx, ny;
    LatLonToNorm(lat, lon, nx, ny);
    double w = WorldSize();
    sx = Width / 2.0 + (nx - CenterX) * w;
    sy = Height / 2.0 + (ny - CenterY) * w;
}

void Viewport::ScreenToLatLon(double sx, double sy, double& lat, double& lon) const {
    double w = WorldSize();
    double nx = CenterX + (sx - Width / 2.0) / w;
    double ny = CenterY + (sy - Height / 2.0) / w;
    NormToLatLon(nx, ny, lat, lon);
}

void Viewport::Pan(double dx, double dy) {
    double w = WorldSize();
    CenterX -= dx / w;
    CenterY -= dy / w;
    ClampCenter();
}

void Viewport::ZoomAt(double sx, double sy, double zoom) {
    double w = WorldSize();
    double px = CenterX + (sx - Width / 2.0) / w;
    double py = CenterY + (sy - Height / 2.0) / w;
    Zoom = ClampZoom(zoom);
    double nw = WorldSize();
    CenterX = px - (sx - Width / 2.0) / nw;
    CenterY = py - (sy - Height / 2.0) / nw;
    ClampCenter();
}

double Viewport::MetersPerPixel(double lat) const {
    const double equatorMetersPer256 = 156543.03392;
    const double Pi = 3.14159265358979323846;
    return equatorMetersPer256 * std::cos(lat * Pi / 180.0) / std::pow(2.0, Zoom);
}

void Viewport::ClampCenter() {
    if (CenterY < 0.0) CenterY = 0.0;
    if (CenterY > 1.0) CenterY = 1.0;
}

std::vector<TileCell> Viewport::VisibleTiles(int margin) const {
    std::vector<TileCell> cells;
    if (Width <= 0.0 || Height <= 0.0) return cells;

    int level = Level();
    int n = 1 << level;
    double w = WorldSize();
    double tileScreen = w / n;  // on-screen size of one level tile (may be fractional)

    double left = CenterX - (Width / 2.0) / w;
    double right = CenterX + (Width / 2.0) / w;
    double top = CenterY - (Height / 2.0) / w;
    double bottom = CenterY + (Height / 2.0) / w;

    int ix0 = static_cast<int>(std::floor(left * n)) - margin;
    int ix1 = static_cast<int>(std::floor(right * n)) + margin;
    int iy0 = static_cast<int>(std::floor(top * n)) - margin;
    int iy1 = static_cast<int>(std::floor(bottom * n)) + margin;
    if (iy0 < 0) iy0 = 0;
    if (iy1 > n - 1) iy1 = n - 1;

    for (int iy = iy0; iy <= iy1; ++iy) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            double x0 = Width / 2.0 + (static_cast<double>(ix) / n - CenterX) * w;
            double y0 = Height / 2.0 + (static_cast<double>(iy) / n - CenterY) * w;
            TileCell c;
            c.z = level;
            c.x = WrapTileX(ix, level);
            c.y = iy;
            c.left = static_cast<int>(std::floor(x0));
            c.top = static_cast<int>(std::floor(y0));
            c.width = static_cast<int>(std::floor(x0 + tileScreen)) - c.left;
            c.height = static_cast<int>(std::floor(y0 + tileScreen)) - c.top;
            if (c.width < 1) c.width = 1;
            if (c.height < 1) c.height = 1;
            cells.push_back(c);
        }
    }

    double cx = Width / 2.0;
    double cy = Height / 2.0;
    std::sort(cells.begin(), cells.end(), [cx, cy](const TileCell& a, const TileCell& b) {
        double ax = a.left + a.width / 2.0 - cx, ay = a.top + a.height / 2.0 - cy;
        double bx = b.left + b.width / 2.0 - cx, by = b.top + b.height / 2.0 - cy;
        return ax * ax + ay * ay < bx * bx + by * by;
    });
    return cells;
}

}  // namespace mappe
