using System;
using System.Globalization;

namespace Mappe.Core
{
    /// <summary>Slippy-map tile helpers (OpenStreetMap z/x/y scheme).</summary>
    public static class TileMath
    {
        public const string OsmTemplate = "https://tile.openstreetmap.org/{zoomlevel}/{x}/{y}.png";
        public const int MaxZoom = 19;

        public static string Build(string template, int zoom, int x, int y)
        {
            if (template == null) throw new ArgumentNullException("template");
            return template
                .Replace("{zoomlevel}", zoom.ToString(CultureInfo.InvariantCulture))
                .Replace("{x}", x.ToString(CultureInfo.InvariantCulture))
                .Replace("{y}", y.ToString(CultureInfo.InvariantCulture));
        }

        public static bool IsValid(int zoom, int x, int y)
        {
            if (zoom < 0 || zoom > MaxZoom) return false;
            int n = 1 << zoom;
            return x >= 0 && x < n && y >= 0 && y < n;
        }

        /// <summary>Wraps x around the antimeridian (the map repeats horizontally).</summary>
        public static int WrapX(int x, int zoom)
        {
            int n = 1 << zoom;
            return ((x % n) + n) % n;
        }

        public static void LatLonToTile(double lat, double lon, int zoom, out int x, out int y)
        {
            double n = Math.Pow(2, zoom);
            double latRad = lat * Math.PI / 180.0;
            x = (int)Math.Floor((lon + 180.0) / 360.0 * n);
            y = (int)Math.Floor((1.0 - Math.Log(Math.Tan(latRad) + 1.0 / Math.Cos(latRad)) / Math.PI) / 2.0 * n);
            int max = (int)n - 1;
            if (x < 0) x = 0;
            if (x > max) x = max;
            if (y < 0) y = 0;
            if (y > max) y = max;
        }

        /// <summary>Key used by the memory cache and by the on-disk file name.</summary>
        public static string Key(int zoom, int x, int y)
        {
            return zoom.ToString(CultureInfo.InvariantCulture) + "_" + x.ToString(CultureInfo.InvariantCulture) + "_" +
                   y.ToString(CultureInfo.InvariantCulture);
        }
    }
}
