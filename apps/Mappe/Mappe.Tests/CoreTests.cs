using System;
using System.Collections.Generic;
using Mappe.Core;
using Xunit;

namespace Mappe.Tests
{
    public class MiniJsonTests
    {
        [Fact]
        public void ParsesNestedObjectsArraysNumbersAndLiterals()
        {
            var root = MiniJson.Parse("{\"a\": [1, -2.5e1, true, false, null], \"b\": {\"c\": \"x\"}}") as Dictionary<string, object>;
            Assert.NotNull(root);
            var a = (List<object>)root["a"];
            Assert.Equal(1.0, (double)a[0]);
            Assert.Equal(-25.0, (double)a[1]);
            Assert.Equal(true, a[2]);
            Assert.Equal(false, a[3]);
            Assert.Null(a[4]);
            Assert.Equal("x", ((Dictionary<string, object>)root["b"])["c"]);
        }

        [Fact]
        public void DecodesEscapesIncludingUnicode()
        {
            var s = (string)MiniJson.Parse("\"a\\\"b\\\\c\\n\\u00e8\\/\"");
            Assert.Equal("a\"b\\c\nè/", s);
        }

        [Fact]
        public void RejectsTrailingDataAndTruncatedInput()
        {
            Assert.Throws<FormatException>(() => MiniJson.Parse("{} x"));
            Assert.Throws<FormatException>(() => MiniJson.Parse("{\"a\":"));
            Assert.Throws<FormatException>(() => MiniJson.Parse("[1,"));
        }
    }

    public class PhotonParserTests
    {
        private const string Sample = @"{
          ""type"": ""FeatureCollection"",
          ""features"": [
            {""type"":""Feature"",
             ""geometry"":{""type"":""Point"",""coordinates"":[12.4964,41.9028]},
             ""properties"":{""osm_id"":1,""name"":""Colosseo"",""street"":""Piazza del Colosseo"",""housenumber"":""1"",
                             ""postcode"":""00184"",""city"":""Roma"",""country"":""Italia"",""osm_key"":""tourism""}},
            {""type"":""Feature"",
             ""geometry"":{""type"":""Point"",""coordinates"":[12.5,41.9]},
             ""properties"":{""street"":""Via Appia Nuova"",""housenumber"":""10"",""town"":""Ciampino"",""country"":""Italia""}}
          ]
        }";

        [Fact]
        public void ParsesNamedAndAddressFeatures()
        {
            List<PlaceResult> r = PhotonParser.Parse(Sample);
            Assert.Equal(2, r.Count);

            Assert.Equal("Colosseo", r[0].Title);
            Assert.Equal("Piazza del Colosseo 1, 00184 Roma, Italia", r[0].Subtitle);
            Assert.Equal(41.9028, r[0].Lat, 4);
            Assert.Equal(12.4964, r[0].Lon, 4);

            Assert.Equal("Via Appia Nuova 10", r[1].Title);
            Assert.Equal("Ciampino, Italia", r[1].Subtitle);
            Assert.Equal("Ciampino", r[1].City);
        }

        [Fact]
        public void SkipsFeaturesWithBadCoordinates()
        {
            const string json = @"{""features"":[
              {""geometry"":{""coordinates"":[200,10]},""properties"":{""name"":""bad""}},
              {""geometry"":{""coordinates"":[""x"",1]},""properties"":{""name"":""bad2""}},
              {""properties"":{""name"":""nogeom""}},
              {""geometry"":{""coordinates"":[9.19,45.46]},""properties"":{""name"":""Duomo""}}]}";
            List<PlaceResult> r = PhotonParser.Parse(json);
            Assert.Single(r);
            Assert.Equal("Duomo", r[0].Title);
        }

        [Fact]
        public void EmptyAndNoFeaturesGiveEmptyList()
        {
            Assert.Empty(PhotonParser.Parse(""));
            Assert.Empty(PhotonParser.Parse(null));
            Assert.Empty(PhotonParser.Parse("{\"features\":[]}"));
            Assert.Empty(PhotonParser.Parse("{\"type\":\"FeatureCollection\"}"));
        }

        [Fact]
        public void MalformedJsonThrows()
        {
            Assert.Throws<FormatException>(() => PhotonParser.Parse("<html>502</html>"));
        }

        [Fact]
        public void UnnamedPlaceFallsBackToCountry()
        {
            var r = new PlaceResult { Country = "Italia" };
            Assert.Equal("Italia", r.Title);
            Assert.Equal("Italia", r.Subtitle);
        }
    }

    public class TileMathTests
    {
        [Fact]
        public void TemplateReplacesAllTokens()
        {
            Assert.Equal("https://tile.openstreetmap.org/6/34/22.png",
                TileMath.Build(TileMath.OsmTemplate, 6, 34, 22));
        }

        [Fact]
        public void OriginTileAtZoomOneIsOneOne()
        {
            int x, y;
            TileMath.LatLonToTile(0, 0, 1, out x, out y);
            Assert.Equal(1, x);
            Assert.Equal(1, y);
        }

        [Fact]
        public void RomeAtZoomSixIsInsideTheGrid()
        {
            int x, y;
            TileMath.LatLonToTile(41.9, 12.5, 6, out x, out y);
            Assert.Equal(34, x);
            Assert.Equal(23, y);
            Assert.True(TileMath.IsValid(6, x, y));
        }

        [Fact]
        public void ValidityBounds()
        {
            Assert.True(TileMath.IsValid(0, 0, 0));
            Assert.False(TileMath.IsValid(0, 1, 0));
            Assert.True(TileMath.IsValid(19, (1 << 19) - 1, 0));
            Assert.False(TileMath.IsValid(20, 0, 0));
            Assert.False(TileMath.IsValid(3, -1, 0));
        }

        [Fact]
        public void WrapsXAroundTheAntimeridian()
        {
            Assert.Equal(7, TileMath.WrapX(-1, 3));
            Assert.Equal(0, TileMath.WrapX(8, 3));
            Assert.Equal(3, TileMath.WrapX(3, 3));
        }

        [Fact]
        public void KeyIsStableAndFileSafe()
        {
            Assert.Equal("6_34_22", TileMath.Key(6, 34, 22));
        }
    }

    public class LruCacheTests
    {
        [Fact]
        public void EvictsLeastRecentlyUsed()
        {
            var c = new LruCache<string, int>(2);
            c.Put("a", 1);
            c.Put("b", 2);
            int v;
            Assert.True(c.TryGet("a", out v)); // a is now most recent
            c.Put("c", 3);                       // evicts b
            Assert.False(c.TryGet("b", out v));
            Assert.True(c.TryGet("a", out v));
            Assert.Equal(1, v);
            Assert.True(c.TryGet("c", out v));
            Assert.Equal(2, c.Count);
        }

        [Fact]
        public void PutOverwritesWithoutGrowing()
        {
            var c = new LruCache<string, int>(2);
            c.Put("a", 1);
            c.Put("a", 5);
            int v;
            Assert.True(c.TryGet("a", out v));
            Assert.Equal(5, v);
            Assert.Equal(1, c.Count);
        }

        [Fact]
        public void TwoHundredTileCapacityHoldsExactlyTwoHundred()
        {
            var c = new LruCache<int, byte[]>(200);
            for (int i = 0; i < 250; i++) c.Put(i, new byte[1]);
            Assert.Equal(200, c.Count);
            byte[] b;
            Assert.False(c.TryGet(0, out b));
            Assert.True(c.TryGet(249, out b));
        }

        [Fact]
        public void RemoveAndClear()
        {
            var c = new LruCache<string, int>(3);
            c.Put("a", 1);
            Assert.True(c.Remove("a"));
            Assert.False(c.Remove("a"));
            c.Put("b", 2);
            c.Clear();
            Assert.Equal(0, c.Count);
        }
    }

    public class SearchDebouncerTests
    {
        [Fact]
        public void FiresOnlyAfterQuietPeriod()
        {
            var d = new SearchDebouncer(400, 3);
            string q;
            d.Touch("rom", 1000);
            Assert.False(d.TryFire(1200, out q));
            d.Touch("roma", 1300);             // typing restarts the delay
            Assert.False(d.TryFire(1600, out q));
            Assert.True(d.TryFire(1700, out q));
            Assert.Equal("roma", q);
            Assert.False(d.TryFire(5000, out q)); // nothing pending any more
        }

        [Fact]
        public void IgnoresShortQueries()
        {
            var d = new SearchDebouncer(400, 3);
            string q;
            d.Touch("ro", 0);
            Assert.False(d.TryFire(1000, out q));
        }

        [Fact]
        public void DoesNotRepeatTheSameQueryUnlessReset()
        {
            var d = new SearchDebouncer(400, 3);
            string q;
            d.Touch("milano", 0);
            Assert.True(d.TryFire(500, out q));
            d.Touch("milano ", 600);           // trimmed: same query
            Assert.False(d.TryFire(1200, out q));
            d.ResetLast();
            d.Touch("milano", 1300);
            Assert.True(d.TryFire(1800, out q));
        }

        [Fact]
        public void CancelDropsPendingQuery()
        {
            var d = new SearchDebouncer(400, 3);
            string q;
            d.Touch("firenze", 0);
            d.Cancel();
            Assert.False(d.TryFire(1000, out q));
        }
    }

    public class GeoUrlsTests
    {
        [Fact]
        public void SearchEncodesQueryAndAddsLocationBias()
        {
            string url = GeoUrls.Search("Caffè Roma", 8, "it", 45.4642, 9.19);
            Assert.Equal("https://photon.komoot.io/api/?q=Caff%C3%A8%20Roma&limit=8&lat=45.4642&lon=9.19", url);
        }

        [Fact]
        public void SearchWithoutPositionHasNoBias()
        {
            string url = GeoUrls.Search("roma", 8, "it", null, null);
            Assert.Equal("https://photon.komoot.io/api/?q=roma&limit=8", url);
        }

        [Fact]
        public void ReverseUsesInvariantDecimalPoint()
        {
            Assert.Equal("https://photon.komoot.io/reverse?lat=41.9&lon=12.5", GeoUrls.Reverse(41.9, 12.5, "it"));
        }

        [Fact]
        public void OsmLinkForBrowser()
        {
            Assert.Equal("metrosurf://www.openstreetmap.org/?mlat=41.9028&mlon=12.4964#map=18/41.9028/12.4964",
                GeoUrls.OsmAppLink(41.9028, 12.4964, 18, "metrosurf"));
        }

        [Fact]
        public void UserAgentHasNoPersonalEmail()
        {
            Assert.Equal("DumbPhoneMappe/0.1 (personal use)", GeoUrls.UserAgent);
            Assert.DoesNotContain("@", GeoUrls.UserAgent);
        }
    }
}
