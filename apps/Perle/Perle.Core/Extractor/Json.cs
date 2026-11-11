/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * C# port of the nanojson accessors used by NewPipeExtractor: missing keys give empty values, never null.
 */

using System.Collections.Generic;
using System.Linq;
using Newtonsoft.Json.Linq;

namespace Perle.Extractor
{
    public static class Json
    {
        static readonly JObject EmptyObject = new JObject();
        static readonly JArray EmptyArray = new JArray();

        public static JObject Obj(this JToken t, string key) => (t as JObject)?[key] as JObject ?? EmptyObject;

        public static JArray Arr(this JToken t, string key) => (t as JObject)?[key] as JArray ?? EmptyArray;

        public static string Str(this JToken t, string key)
        {
            var v = (t as JObject)?[key];
            return v != null && v.Type == JTokenType.String ? (string)v : null;
        }

        public static bool Bool(this JToken t, string key)
        {
            var v = (t as JObject)?[key];
            return v != null && v.Type == JTokenType.Boolean && (bool)v;
        }

        public static long Long(this JToken t, string key, long fallback = -1)
        {
            var v = (t as JObject)?[key];
            if (v == null) return fallback;
            if (v.Type == JTokenType.Integer) return (long)v;
            long l;
            return v.Type == JTokenType.String && long.TryParse((string)v, out l) ? l : fallback;
        }

        public static bool Has(this JToken t, string key) => (t as JObject)?[key] != null;

        /// <summary>Like JsonUtils.getObject(object, "a.b.c").</summary>
        public static JObject ObjPath(this JToken t, string path)
        {
            foreach (var k in path.Split('.')) t = t.Obj(k);
            return (JObject)t;
        }

        public static JArray ArrPath(this JToken t, string path)
        {
            var keys = path.Split('.');
            for (int i = 0; i < keys.Length - 1; i++) t = t.Obj(keys[i]);
            return t.Arr(keys[keys.Length - 1]);
        }

        public static string StrPath(this JToken t, string path)
        {
            var keys = path.Split('.');
            for (int i = 0; i < keys.Length - 1; i++) t = t.Obj(keys[i]);
            return t.Str(keys[keys.Length - 1]);
        }

        public static IEnumerable<JObject> Objects(this JArray a) => a.OfType<JObject>();

        /// <summary>Text of a YouTube "runs"/"simpleText"/"content" text object.</summary>
        public static string Text(this JToken textObject)
        {
            var o = textObject as JObject;
            if (o == null) return null;
            var simple = o.Str("simpleText") ?? o.Str("content");
            if (simple != null) return simple;
            var runs = o.Arr("runs");
            if (runs.Count == 0) return null;
            return string.Concat(runs.Objects().Select(r => r.Str("text")));
        }
    }
}
