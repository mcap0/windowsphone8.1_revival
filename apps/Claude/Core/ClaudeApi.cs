using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace Claude.Core
{
    /// <summary>A model that can be chosen in the settings.</summary>
    public sealed class ClaudeModel
    {
        public string Id { get; }
        public string Name { get; }
        public ClaudeModel(string id, string name) { Id = id; Name = name; }
        public override string ToString() => Name;

        public static readonly ClaudeModel[] All =
        {
            new ClaudeModel("claude-opus-5-5", "Claude Opus 5.5 (il più capace)"),
            new ClaudeModel("claude-sonnet-5-5", "Claude Sonnet 5.5 (veloce)"),
            new ClaudeModel("claude-haiku-5-5", "Claude Haiku 5.5 (il più economico)")
        };

        public const string DefaultId = "claude-opus-5-5";
    }

    public sealed class ClaudeException : Exception
    {
        public string ErrorType { get; }
        public ClaudeException(string errorType, string message) : base(message) { ErrorType = errorType; }
    }

    /// <summary>What happens while an answer is streamed, as handled by the UI.</summary>
    public interface IStreamListener
    {
        /// <summary>A piece of the answer text.</summary>
        void OnText(string text);

        /// <summary>Claude started a web search for the given query.</summary>
        void OnSearch(string query);
    }

    /// <summary>
    /// Client of the Claude Messages API (https://docs.anthropic.com/en/api/messages), with
    /// streaming: the answer appears while it is written, which matters on a slow phone network.
    /// </summary>
    public sealed class ClaudeApi
    {
        public const string Endpoint = "https://api.anthropic.com/v1/messages";
        public const string ApiVersion = "2023-06-01";
        public const int MaxTokens = 8192;

        readonly HttpClient http;

        public ClaudeApi(HttpClient http = null)
        {
            this.http = http ?? new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        }

        public static JObject BuildRequest(string model, string system, IList<ChatMessage> messages, bool webSearch)
        {
            var array = new JArray();
            foreach (var m in messages)
            {
                if (string.IsNullOrEmpty(m.Text)) continue;
                array.Add(new JObject { ["role"] = m.Role, ["content"] = m.Text });
            }
            var request = new JObject
            {
                ["model"] = model,
                ["max_tokens"] = MaxTokens,
                ["stream"] = true,
                ["messages"] = array
            };
            if (!string.IsNullOrEmpty(system)) request["system"] = system;
            if (webSearch)
                request["tools"] = new JArray { new JObject { ["type"] = "web_search_20250305", ["name"] = "web_search", ["max_uses"] = 5 } };
            return request;
        }

        /// <summary>Sends the conversation and streams the answer to listener.</summary>
        /// <returns>the whole answer text and the stop reason</returns>
        public async Task<StreamResult> SendAsync(string apiKey, JObject request, IStreamListener listener, CancellationToken ct)
        {
            var message = new HttpRequestMessage(HttpMethod.Post, Endpoint)
            {
                Content = new StringContent(request.ToString(Newtonsoft.Json.Formatting.None), Encoding.UTF8, "application/json")
            };
            message.Headers.TryAddWithoutValidation("x-api-key", apiKey);
            message.Headers.TryAddWithoutValidation("anthropic-version", ApiVersion);
            using (var response = await http.SendAsync(message, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false))
            {
                if (!response.IsSuccessStatusCode)
                {
                    var body = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                    throw ErrorFrom((int)response.StatusCode, body);
                }
                using (var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false))
                using (var reader = new StreamReader(stream, Encoding.UTF8))
                {
                    var parser = new SseParser(listener);
                    string line;
                    while ((line = await reader.ReadLineAsync().ConfigureAwait(false)) != null)
                    {
                        ct.ThrowIfCancellationRequested();
                        parser.Line(line);
                        if (parser.Finished) break;
                    }
                    return parser.Result();
                }
            }
        }

        public static ClaudeException ErrorFrom(int status, string body)
        {
            try
            {
                var error = JObject.Parse(body)["error"];
                if (error != null) return new ClaudeException((string)error["type"], Describe((string)error["type"], (string)error["message"]));
            }
            catch (Exception) { }
            return new ClaudeException("http_" + status, "Errore HTTP " + status);
        }

        static string Describe(string type, string message)
        {
            switch (type)
            {
                case "authentication_error": return "Chiave API non valida. Controllala nelle impostazioni.";
                case "permission_error": return "La chiave API non ha i permessi per questo modello.";
                case "rate_limit_error": return "Troppe richieste: riprova tra poco.";
                case "overloaded_error": return "Claude è sovraccarico in questo momento: riprova tra poco.";
                default: return message ?? type;
            }
        }
    }

    public sealed class StreamResult
    {
        public string Text { get; set; }
        public string StopReason { get; set; }
    }

    /// <summary>Parses the server-sent events of a streamed message.</summary>
    public sealed class SseParser
    {
        readonly IStreamListener listener;
        readonly StringBuilder text = new StringBuilder();
        readonly StringBuilder data = new StringBuilder();
        // the JSON input of the server tool being streamed, e.g. {"query": "..."}
        readonly StringBuilder toolInput = new StringBuilder();
        bool inSearch;
        string stopReason;

        public bool Finished { get; private set; }

        public SseParser(IStreamListener listener) { this.listener = listener; }

        public void Line(string line)
        {
            if (line.Length == 0)
            {
                // an empty line ends the event
                if (data.Length > 0) Event(data.ToString());
                data.Clear();
                return;
            }
            if (line.StartsWith("data:"))
            {
                if (data.Length > 0) data.Append('\n');
                data.Append(line.Substring(5).TrimStart(' '));
            }
            // "event:" lines repeat the type which is also in the data
        }

        void Event(string json)
        {
            var e = JObject.Parse(json);
            switch ((string)e["type"])
            {
                case "content_block_start":
                    var block = e["content_block"];
                    if ((string)block?["type"] == "server_tool_use" && (string)block["name"] == "web_search")
                    {
                        inSearch = true;
                        toolInput.Clear();
                    }
                    else if ((string)block?["type"] == "text" && text.Length > 0 && !text.ToString().EndsWith("\n"))
                    {
                        // text after a search continues in a new paragraph
                        Append("\n\n");
                    }
                    break;
                case "content_block_delta":
                    var delta = e["delta"];
                    var deltaType = (string)delta?["type"];
                    if (deltaType == "text_delta") Append((string)delta["text"]);
                    else if (deltaType == "input_json_delta" && inSearch) toolInput.Append((string)delta["partial_json"]);
                    break;
                case "content_block_stop":
                    if (inSearch)
                    {
                        inSearch = false;
                        string query = null;
                        try { query = (string)JObject.Parse(toolInput.ToString())["query"]; } catch (Exception) { }
                        listener?.OnSearch(query ?? "");
                    }
                    break;
                case "message_delta":
                    stopReason = (string)e["delta"]?["stop_reason"] ?? stopReason;
                    break;
                case "message_stop":
                    Finished = true;
                    break;
                case "error":
                    var error = e["error"];
                    throw ClaudeApi.ErrorFrom(0, new JObject { ["error"] = error }.ToString());
            }
        }

        void Append(string s)
        {
            if (string.IsNullOrEmpty(s)) return;
            text.Append(s);
            listener?.OnText(s);
        }

        public StreamResult Result() => new StreamResult { Text = text.ToString(), StopReason = stopReason };
    }
}
