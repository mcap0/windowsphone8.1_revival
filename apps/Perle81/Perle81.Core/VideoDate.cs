/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Upload year of a video (©day of the audio tags): the microformat of the WEB player response. The response can
 * be UNPLAYABLE for the WEB client and still carry the microformat.
 */

using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Perle.Extractor;

namespace Perle.Progressive
{
    public static class VideoDate
    {
        /// <returns>the year, e.g. "2009", or null when YouTube does not say</returns>
        public static async Task<string> GetYearAsync(string videoId, CancellationToken ct = default(CancellationToken))
        {
            var body = Innertube.PrepareBody(Innertube.WebClient());
            body["videoId"] = videoId;
            body["contentCheckOk"] = true;
            body["racyCheckOk"] = true;
            var json = await Innertube.PostAsync(Innertube.YoutubeiV1Url + "player?" + Innertube.DisablePrettyPrint, body, false, ct)
                .ConfigureAwait(false);
            string date = json.ObjPath("microformat.playerMicroformatRenderer").Str("publishDate")
                ?? json.ObjPath("microformat.playerMicroformatRenderer").Str("uploadDate");
            return date != null && date.Length >= 4 ? date.Substring(0, 4) : null;
        }
    }
}
