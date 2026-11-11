// Pure map viewport: centre, fractional zoom and the visible tile grid. No WinRT here.
#pragma once

#include <vector>

namespace mappe {

// One tile to draw, in screen pixels. x is wrapped, left/top are unwrapped positions.
struct TileCell {
    int z, x, y;
    int left, top, width, height;
};

class Viewport {
public:
    static const double MinZoom;
    static const double MaxZoom;

    // Centre in normalized Web Mercator coordinates. CenterX is not wrapped (it can drift past 1).
    double CenterX = 0.5;
    double CenterY = 0.5;
    double Zoom = 6.0;
    double Width = 0.0;
    double Height = 0.0;

    double ClampZoom(double z) const;
    // Integer level whose tiles are drawn (floor of the zoom).
    int Level() const;
    // Screen scale of the level tiles: 2^(Zoom - Level), in [1,2).
    double Scale() const;
    // Size of the world in screen pixels at the current zoom.
    double WorldSize() const;

    void SetCenterLatLon(double lat, double lon);
    void CenterLatLon(double& lat, double& lon) const;

    void ToScreen(double lat, double lon, double& sx, double& sy) const;
    void ScreenToLatLon(double sx, double sy, double& lat, double& lon) const;

    // Moves the map by a screen-pixel delta (finger drag).
    void Pan(double dx, double dy);
    // Changes zoom keeping the point under (sx,sy) fixed.
    void ZoomAt(double sx, double sy, double zoom);

    // Metres per screen pixel at the latitude (256-px tiles).
    double MetersPerPixel(double lat) const;

    // Tiles covering the viewport plus 'margin' tiles on every side, nearest to the centre first.
    std::vector<TileCell> VisibleTiles(int margin) const;

private:
    void ClampCenter();
};

}  // namespace mappe
