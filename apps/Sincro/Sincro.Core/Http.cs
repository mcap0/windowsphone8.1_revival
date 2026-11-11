using Windows.Web.Http;
using Windows.Web.Http.Filters;

namespace Sincro.Core
{
    /// <summary>HttpClient factory that bypasses the WinINet cache (WP8.1 caches GETs by default).</summary>
    public static class Http
    {
        public static HttpClient NoCacheClient()
        {
            var filter = new HttpBaseProtocolFilter();
            filter.CacheControl.ReadBehavior = HttpCacheReadBehavior.MostRecent;
            filter.CacheControl.WriteBehavior = HttpCacheWriteBehavior.NoCache;
            return new HttpClient(filter);
        }
    }
}
