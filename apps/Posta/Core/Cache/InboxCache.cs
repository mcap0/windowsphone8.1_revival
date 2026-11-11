using System;
using System.Collections.Generic;
using Newtonsoft.Json;
using Posta.Core.Model;

namespace Posta.Core.Cache
{
    /// <summary>What is kept on disk so the inbox can be shown before the network answers.</summary>
    public sealed class InboxCacheDocument
    {
        public string Account { get; set; } = "";
        public int Total { get; set; }
        public List<MailSummary> Messages { get; set; } = new List<MailSummary>();
    }

    /// <summary>JSON (de)serialization of the inbox cache. File I/O lives in the app project.</summary>
    public static class InboxCache
    {
        public static string Serialize(InboxCacheDocument doc)
        {
            return JsonConvert.SerializeObject(doc ?? new InboxCacheDocument());
        }

        /// <summary>Returns null when the text is empty or corrupt (the cache is then simply ignored).</summary>
        public static InboxCacheDocument Deserialize(string json)
        {
            if (string.IsNullOrWhiteSpace(json)) return null;
            try
            {
                var doc = JsonConvert.DeserializeObject<InboxCacheDocument>(json);
                if (doc == null || doc.Messages == null) return null;
                return doc;
            }
            catch (JsonException)
            {
                return null;
            }
        }
    }
}
