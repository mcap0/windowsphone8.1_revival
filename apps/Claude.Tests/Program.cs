using System;
using System.Collections.Generic;
using Claude.Core;

static class Program
{
    static int failures;

    static void Check(bool ok, string name)
    {
        Console.WriteLine((ok ? "  ok   " : "  FAIL ") + name);
        if (!ok) failures++;
    }

    sealed class Recorder : IStreamListener
    {
        public readonly List<string> Events = new List<string>();
        public void OnText(string text) => Events.Add("text:" + text);
        public void OnSearch(string query) => Events.Add("search:" + query);
    }

    static Recorder Parse(string sse, out StreamResult result)
    {
        var recorder = new Recorder();
        var parser = new SseParser(recorder);
        foreach (var line in sse.Replace("\r", "").Split('\n')) { parser.Line(line); if (parser.Finished) break; }
        result = parser.Result();
        return recorder;
    }

    static int Main()
    {
        var messages = new List<ChatMessage> { new ChatMessage { Role = "user", Text = "Ciao" }, new ChatMessage { Role = "assistant", Text = "" } };
        var request = ClaudeApi.BuildRequest("claude-opus-5-5", "sys", messages, true);
        Check((string)request["model"] == "claude-opus-5-5" && (bool)request["stream"] && (string)request["system"] == "sys", "request fields");
        Check(((Newtonsoft.Json.Linq.JArray)request["messages"]).Count == 1, "empty messages are skipped");
        Check((string)request["tools"][0]["type"] == "web_search_20250305", "web search tool");
        Check(ClaudeApi.BuildRequest("m", null, messages, false)["tools"] == null, "no tools without web search");

        var sse = @"event: message_start
data: {""type"":""message_start"",""message"":{""id"":""msg_1"",""type"":""message"",""role"":""assistant"",""content"":[]}}

event: content_block_start
data: {""type"":""content_block_start"",""index"":0,""content_block"":{""type"":""server_tool_use"",""id"":""srvtoolu_1"",""name"":""web_search"",""input"":{}}}

event: content_block_delta
data: {""type"":""content_block_delta"",""index"":0,""delta"":{""type"":""input_json_delta"",""partial_json"":""{\""query\"": \""meteo""}}

event: content_block_delta
data: {""type"":""content_block_delta"",""index"":0,""delta"":{""type"":""input_json_delta"",""partial_json"":"" Roma\""}""}}

event: content_block_stop
data: {""type"":""content_block_stop"",""index"":0}

event: content_block_start
data: {""type"":""content_block_start"",""index"":1,""content_block"":{""type"":""web_search_tool_result"",""tool_use_id"":""srvtoolu_1"",""content"":[]}}

event: content_block_stop
data: {""type"":""content_block_stop"",""index"":1}

event: ping
data: {""type"": ""ping""}

event: content_block_start
data: {""type"":""content_block_start"",""index"":2,""content_block"":{""type"":""text"",""text"":""""}}

event: content_block_delta
data: {""type"":""content_block_delta"",""index"":2,""delta"":{""type"":""text_delta"",""text"":""Oggi a Roma ""}}

event: content_block_delta
data: {""type"":""content_block_delta"",""index"":2,""delta"":{""type"":""text_delta"",""text"":""c'è il sole.""}}

event: content_block_stop
data: {""type"":""content_block_stop"",""index"":2}

event: message_delta
data: {""type"":""message_delta"",""delta"":{""stop_reason"":""end_turn"",""stop_sequence"":null},""usage"":{""output_tokens"":12}}

event: message_stop
data: {""type"":""message_stop""}

";
        var recorder = Parse(sse, out var result);
        Check(string.Join("|", recorder.Events) == "search:meteo Roma|text:Oggi a Roma |text:c'è il sole.", "stream events: " + string.Join("|", recorder.Events));
        Check(result.Text == "Oggi a Roma c'è il sole." && result.StopReason == "end_turn", "result");

        try
        {
            Parse("event: error\ndata: {\"type\": \"error\", \"error\": {\"type\": \"overloaded_error\", \"message\": \"Overloaded\"}}\n\n", out _);
            Check(false, "stream error throws");
        }
        catch (ClaudeException e) { Check(e.ErrorType == "overloaded_error", "stream error throws"); }

        var auth = ClaudeApi.ErrorFrom(401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}");
        Check(auth.ErrorType == "authentication_error" && auth.Message.Contains("Chiave API"), "http error");
        Check(ClaudeApi.ErrorFrom(502, "<html>").Message == "Errore HTTP 502", "non JSON error");

        var history = new ChatHistory();
        var c = new Conversation { Title = Conversation.TitleFor("una domanda") };
        c.Messages.Add(new ChatMessage { Role = "user", Text = "una domanda" });
        history.Touch(c);
        var back = ChatHistory.Parse(history.ToJson());
        Check(back.Conversations.Count == 1 && back.Conversations[0].Messages[0].Text == "una domanda" && back.Conversations[0].Title == "una domanda", "history round trip");
        Check(ChatHistory.Parse("not json").Conversations.Count == 0, "broken history is ignored");
        Check(Conversation.TitleFor(new string('a', 100)).Length == 58, "long titles are cut");

        Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        return failures;
    }
}
