#include "TileMath.h"

#include <cmath>

namespace mappe {

namespace {
const double Pi = 3.14159265358979323846;
const double MaxLat = 85.0511287798;

std::string ReplaceAll(std::string s, const std::string& what, const std::string& with) {
    std::string::size_type pos = 0;
    while ((pos = s.find(what, pos)) != std::string::npos) {
        s.replace(pos, what.size(), with);
        pos += with.size();
    }
    return s;
}
}  // namespace

std::int64_t TileKey(int z, int x, int y) {
    return (static_cast<std::int64_t>(z) << 42) | (static_cast<std::int64_t>(x) << 21) |
           static_cast<std::int64_t>(y);
}

void DecodeTileKey(std::int64_t key, int& z, int& x, int& y) {
    z = static_cast<int>(key >> 42);
    x = static_cast<int>((key >> 21) & ((1 << 21) - 1));
    y = static_cast<int>(key & ((1 << 21) - 1));
}

std::string TileFileName(int z, int x, int y) {
    return std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".png";
}

int WrapTileX(int x, int zoom) {
    int n = 1 << zoom;
    return ((x % n) + n) % n;
}

bool IsValidTile(int z, int x, int y) {
    if (z < 0 || z > MaxTileZoom) return false;
    int n = 1 << z;
    return x >= 0 && x < n && y >= 0 && y < n;
}

std::string BuildTileUrl(const std::string& templ, int z, int x, int y) {
    std::string s = ReplaceAll(templ, "{z}", std::to_string(z));
    s = ReplaceAll(s, "{x}", std::to_string(x));
    return ReplaceAll(s, "{y}", std::to_string(y));
}

void LatLonToNorm(double lat, double lon, double& nx, double& ny) {
    if (lat > MaxLat) lat = MaxLat;
    if (lat < -MaxLat) lat = -MaxLat;
    double latRad = lat * Pi / 180.0;
    nx = (lon + 180.0) / 360.0;
    ny = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / Pi) / 2.0;
}

void NormToLatLon(double nx, double ny, double& lat, double& lon) {
    lon = nx * 360.0 - 180.0;
    double n = Pi * (1.0 - 2.0 * ny);
    lat = std::atan(std::sinh(n)) * 180.0 / Pi;
}

void LatLonToTile(double lat, double lon, int zoom, int& x, int& y) {
    double nx, ny;
    LatLonToNorm(lat, lon, nx, ny);
    double n = std::pow(2.0, zoom);
    x = static_cast<int>(std::floor(nx * n));
    y = static_cast<int>(std::floor(ny * n));
    int max = static_cast<int>(n) - 1;
    if (x < 0) x = 0;
    if (x > max) x = max;
    if (y < 0) y = 0;
    if (y > max) y = max;
}

bool FindFallback(int z, int x, int y, const std::function<bool(int, int, int)>& ready,
                  int maxDepth, TileFallback& out) {
    for (int d = 1; d <= maxDepth; ++d) {
        int pz = z - d;
        if (pz < 0) break;
        int ax = x >> d;
        int ay = y >> d;
        if (ready(pz, ax, ay)) {
            out.depth = d;
            out.z = pz;
            out.x = ax;
            out.y = ay;
            out.subX = x & ((1 << d) - 1);
            out.subY = y & ((1 << d) - 1);
            return true;
        }
    }
    return false;
}

}  // namespace mappe
