using System.IO;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace Sincro.Logic
{
    /// <summary>
    /// Parses Google JSON keeping date-times as the original strings. The default JObject.Parse turns
    /// "2026-10-20T09:00:00+02:00" into a local DateTime and loses the event's own UTC offset.
    /// </summary>
    public static class GoogleJsonReader
    {
        public static JObject Parse(string text)
        {
            using (var reader = new JsonTextReader(new StringReader(text)))
            {
                reader.DateParseHandling = DateParseHandling.None;
                return (JObject)JToken.ReadFrom(reader);
            }
        }
    }
}
