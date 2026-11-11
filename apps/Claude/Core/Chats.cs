using System;
using System.Collections.Generic;
using System.Linq;
using Newtonsoft.Json;

namespace Claude.Core
{
    public sealed class ChatMessage
    {
        /// <summary>"user" or "assistant"</summary>
        public string Role { get; set; }
        public string Text { get; set; }

        [JsonIgnore] public bool IsUser => Role == "user";
    }

    public sealed class Conversation
    {
        public string Id { get; set; } = Guid.NewGuid().ToString("N");
        public string Title { get; set; } = "";
        public long Updated { get; set; }
        public List<ChatMessage> Messages { get; set; } = new List<ChatMessage>();

        /// <summary>The first words of the first question.</summary>
        public static string TitleFor(string firstQuestion)
        {
            var t = (firstQuestion ?? "").Replace('\n', ' ').Trim();
            return t.Length <= 60 ? t : t.Substring(0, 57) + "…";
        }
    }

    /// <summary>All the conversations, newest first, saved as one JSON file.</summary>
    public sealed class ChatHistory
    {
        public const int MaxConversations = 200;

        public List<Conversation> Conversations { get; set; } = new List<Conversation>();

        public static ChatHistory Parse(string json)
        {
            if (string.IsNullOrWhiteSpace(json)) return new ChatHistory();
            try { return JsonConvert.DeserializeObject<ChatHistory>(json) ?? new ChatHistory(); }
            catch (JsonException) { return new ChatHistory(); }
        }

        public string ToJson() => JsonConvert.SerializeObject(this);

        /// <summary>Moves conversation to the top, adding it if new, and drops the oldest ones.</summary>
        public void Touch(Conversation conversation)
        {
            conversation.Updated = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            Conversations.Remove(conversation);
            Conversations.Insert(0, conversation);
            if (Conversations.Count > MaxConversations) Conversations.RemoveRange(MaxConversations, Conversations.Count - MaxConversations);
        }

        public Conversation Find(string id) => Conversations.FirstOrDefault(c => c.Id == id);
    }
}
