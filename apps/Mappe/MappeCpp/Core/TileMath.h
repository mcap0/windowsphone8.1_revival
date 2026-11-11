// Pure tile math (OpenStreetMap slippy-map scheme). Standard C++ only: no WinRT here.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mappe {

const int TileSize = 256;
const int MaxTileZoom = 19;

// Key packing z/x/y into one 64-bit integer (x and y fit in 21 bits up to zoom 19).
std::int64_t TileKey(int z, int x, int y);
void DecodeTileKey(std::int64_t key, int& z, int& x, int& y);

// File name used by the disk cache: "z_x_y.png".
std::string TileFileName(int z, int x, int y);

// Wraps x around the antimeridian (the world repeats horizontally).
int WrapTileX(int x, int zoom);
bool IsValidTile(int z, int x, int y);

// Replaces {z} {x} {y} in the template.
std::string BuildTileUrl(const std::string& templ, int z, int x, int y);

// Normalized Web Mercator coordinates in [0,1): nx grows eastwards, ny grows southwards.
void LatLonToNorm(double lat, double lon, double& nx, double& ny);
void NormToLatLon(double nx, double ny, double& lat, double& lon);

// Tile containing a coordinate at an integer zoom.
void LatLonToTile(double lat, double lon, int zoom, int& x, int& y);

// Nearest cached ancestor of a missing tile. The child (x,y) lies inside the ancestor
// at depth 'depth', in the sub-square (subX, subY) of 2^depth x 2^depth pieces.
struct TileFallback {
    int depth;
    int z, x, y;
    int subX, subY;
};

// Looks for the closest ancestor (depth 1..maxDepth) for which ready(z,x,y) is true.
// x must already be wrapped. Returns false when none is found.
bool FindFallback(int z, int x, int y, const std::function<bool(int, int, int)>& ready,
                  int maxDepth, TileFallback& out);

}  // namespace mappe
