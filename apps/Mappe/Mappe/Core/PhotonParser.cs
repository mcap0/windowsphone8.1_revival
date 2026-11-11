using System;
using System.Collections.Generic;

namespace Mappe.Core
{
    public sealed class PlaceResult
    {
        public string Name;
        public string Street;
        public string HouseNumber;
        public string City;
        public string Postcode;
        public string Country;
        public double Lat;
        public double Lon;

        /// <summary>First line shown in the list.</summary>
        public string Title
        {
            get
            {
                if (!string.IsNullOrEmpty(Name)) return Name;
                if (!string.IsNullOrEmpty(Street))
                    return string.IsNullOrEmpty(HouseNumber) ? Street : Street + " " + HouseNumber;
                if (!string.IsNullOrEmpty(City)) return City;
                return Country ?? "(senza nome)";
            }
        }

        /// <summary>Second line: street and number (if not already in the title), postcode/city, country.</summary>
        public string Subtitle
        {
            get
            {
                var parts = new List<string>();
                if (!string.IsNullOrEmpty(Name) && !string.IsNullOrEmpty(Street))
                    parts.Add(string.IsNullOrEmpty(HouseNumber) ? Street : Street + " " + HouseNumber);
                string cityPart = string.Join(" ", Filter(Postcode, City));
                if (cityPart.Length > 0) parts.Add(cityPart);
                if (!string.IsNullOrEmpty(Country)) parts.Add(Country);
                return string.Join(", ", parts);
            }
        }

        private static IEnumerable<string> Filter(params string[] values)
        {
            foreach (string v in values)
                if (!string.IsNullOrEmpty(v)) yield return v;
        }
    }

    /// <summary>Parses Photon (GeoJSON FeatureCollection) responses.</summary>
    public static class PhotonParser
    {
        public static List<PlaceResult> Parse(string json)
        {
            var list = new List<PlaceResult>();
            if (string.IsNullOrWhiteSpace(json)) return list;

            var root = MiniJson.Parse(json) as Dictionary<string, object>;
            if (root == null) throw new FormatException("Photon: risposta non e' un oggetto JSON");
            var features = root.ContainsKey("features") ? root["features"] as List<object> : null;
            if (features == null) return list;

            foreach (object item in features)
            {
                var feature = item as Dictionary<string, object>;
                if (feature == null) continue;

                var geometry = Obj(feature, "geometry");
                var coords = geometry == null ? null : geometry["coordinates"] as List<object>;
                if (coords == null || coords.Count < 2) continue;
                if (!(coords[0] is double) || !(coords[1] is double)) continue;
                double lon = (double)coords[0];
                double lat = (double)coords[1];
                if (lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;

                var props = Obj(feature, "properties") ?? new Dictionary<string, object>();
                var place = new PlaceResult
                {
                    Lat = lat,
                    Lon = lon,
                    Name = Str(props, "name"),
                    Street = Str(props, "street"),
                    HouseNumber = Str(props, "housenumber"),
                    City = FirstNonEmpty(Str(props, "city"), Str(props, "town"), Str(props, "village"), Str(props, "locality"), Str(props, "county")),
                    Postcode = Str(props, "postcode"),
                    Country = Str(props, "country"),
                };
                list.Add(place);
            }
            return list;
        }

        private static Dictionary<string, object> Obj(Dictionary<string, object> d, string key)
        {
            object v;
            return d.TryGetValue(key, out v) ? v as Dictionary<string, object> : null;
        }

        private static string Str(Dictionary<string, object> d, string key)
        {
            object v;
            if (!d.TryGetValue(key, out v)) return null;
            string s = v as string;
            return string.IsNullOrWhiteSpace(s) ? null : s.Trim();
        }

        private static string FirstNonEmpty(params string[] values)
        {
            foreach (string v in values)
                if (!string.IsNullOrEmpty(v)) return v;
            return null;
        }
    }
}
