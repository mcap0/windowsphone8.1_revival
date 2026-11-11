using System;
using System.Globalization;

namespace Mappe.Core
{
    /// <summary>URL builders for the Photon geocoder and OpenStreetMap links.</summary>
    public static class GeoUrls
    {
        /// <summary>Sent with every HTTP request, as required by the OSM tile usage policy.</summary>
        public const string UserAgent = "DumbPhoneMappe/0.1 (personal use)";
        public const string PhotonSearch = "https://photon.komoot.io/api/";
        public const string PhotonReverse = "https://photon.komoot.io/reverse";

        public static string Search(string query, int limit, string lang, double? biasLat, double? biasLon)
        {
            if (query == null) throw new ArgumentNullException("query");
            string url = PhotonSearch + "?q=" + Uri.EscapeDataString(query) +
                         "&limit=" + limit.ToString(CultureInfo.InvariantCulture) +
                         LangParam(lang);
            if (biasLat.HasValue && biasLon.HasValue)
                url += "&lat=" + Num(biasLat.Value) + "&lon=" + Num(biasLon.Value);
            return url;
        }

        public static string Reverse(double lat, double lon, string lang)
        {
            return PhotonReverse + "?lat=" + Num(lat) + "&lon=" + Num(lon) + LangParam(lang);
        }

        // The public Photon instance answers 400 for languages other than these; without
        // the parameter it returns local names (Italian names in Italy).
        private static string LangParam(string lang)
        {
            if (lang == "en" || lang == "de" || lang == "fr")
                return "&lang=" + lang;
            return "";
        }

        /// <summary>Deep link that opens the same spot in the browser (MetroSurf scheme first, see MainPage).</summary>
        public static string OsmAppLink(double lat, double lon, int zoom, string scheme)
        {
            string la = Num(lat), lo = Num(lon);
            return scheme + "://www.openstreetmap.org/?mlat=" + la + "&mlon=" + lo +
                   "#map=" + zoom.ToString(CultureInfo.InvariantCulture) + "/" + la + "/" + lo;
        }

        public static string Num(double v)
        {
            return v.ToString("0.#####", CultureInfo.InvariantCulture);
        }
    }
}
