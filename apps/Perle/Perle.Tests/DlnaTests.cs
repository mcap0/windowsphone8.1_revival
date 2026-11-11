// Ports of the DLNA unit tests of NewPipe Perle (app/src/test/java/org/schabi/newpipe/cast/dlna).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;
using Perle.Cast;
using Perle.Cast.Dlna;

static class DlnaTests
{
    static KeyValuePair<string, string> P(string k, string v) => new KeyValuePair<string, string>(k, v);

    public static void SsdpTest()
    {
        Console.WriteLine("SsdpTest");
        T.Run("parsesSamsungSearchAnswer", () =>
        {
            var answer = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nDATE: Tue, 13 Oct 2026 10:00:00 GMT\r\nEXT:\r\n"
                + "LOCATION: http://192.168.1.50:9197/dmr\r\nSERVER: SHP, UPnP/1.0, Samsung UPnP SDK/1.0\r\n"
                + "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                + "USN: uuid:0a1b2c3d-0000-1000-8000-aabbccddeeff::urn:schemas-upnp-org:device:MediaRenderer:1\r\nContent-Length: 0\r\n\r\n";
            var r = Ssdp.ParseResponse(answer);
            T.Eq("http://192.168.1.50:9197/dmr", r.Location);
            T.Eq(Ssdp.MediaRenderer, r.SearchTarget);
            T.True(r.Usn.StartsWith("uuid:0a1b2c3d"));
            T.Eq("SHP, UPnP/1.0, Samsung UPnP SDK/1.0", r.Server);
        });
        T.Run("headerNamesAreCaseInsensitiveAndLfOnlyIsAccepted", () =>
        {
            var r = Ssdp.ParseResponse("HTTP/1.1 200 OK\nLocation:   http://10.0.0.2:49152/description.xml  \nst: urn:schemas-upnp-org:service:AVTransport:1\n\n");
            T.Eq("http://10.0.0.2:49152/description.xml", r.Location);
            T.Eq(Ssdp.AvTransport, r.SearchTarget);
            T.Eq(null, r.Usn);
        });
        T.Run("locationValueWithColonsIsKeptWhole", () =>
            T.Eq("http://[fe80::1]:8080/a", Ssdp.ParseResponse("HTTP/1.1 200 OK\r\nLOCATION: http://[fe80::1]:8080/a\r\n\r\n").Location));
        T.Run("parsesNotifyAliveAndIgnoresByeBye", () =>
        {
            var alive = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: urn:schemas-upnp-org:device:MediaRenderer:1\r\nNTS: ssdp:alive\r\nLOCATION: http://192.168.1.50:9197/dmr\r\n\r\n";
            T.Eq(Ssdp.MediaRenderer, Ssdp.ParseResponse(alive).SearchTarget);
            T.Eq(null, Ssdp.ParseResponse(alive.Replace("ssdp:alive", "ssdp:byebye")));
        });
        T.Run("rejectsInvalidAnswers", () =>
        {
            T.Eq(null, Ssdp.ParseResponse(""));
            T.Eq(null, Ssdp.ParseResponse("HTTP/1.1 404 Not Found\r\nLOCATION: http://a/b\r\n\r\n"));
            T.Eq(null, Ssdp.ParseResponse("HTTP/1.1 200 OK\r\nST: x\r\n\r\n"));
            T.Eq(null, Ssdp.ParseResponse("M-SEARCH * HTTP/1.1\r\nLOCATION: http://a/b\r\n\r\n"));
        });
        T.Run("buildsSearchRequest", () =>
        {
            var request = Ssdp.BuildSearchRequest(Ssdp.MediaRenderer, 3);
            var lines = request.Split(new[] { "\r\n" }, StringSplitOptions.None);
            T.Eq("M-SEARCH * HTTP/1.1", lines[0]);
            T.True(lines.Contains("HOST: 239.255.255.250:1900"));
            T.True(lines.Contains("MAN: \"ssdp:discover\""));
            T.True(lines.Contains("MX: 3"));
            T.True(lines.Contains("ST: urn:schemas-upnp-org:device:MediaRenderer:1"));
            T.True(request.EndsWith("\r\n\r\n"));
        });
    }

    const string Avt = "urn:schemas-upnp-org:service:AVTransport:1";
    const string Rc = "urn:schemas-upnp-org:service:RenderingControl:1";

    public static void SoapTest()
    {
        Console.WriteLine("SoapTest");
        T.Run("buildsEnvelopeWithArgumentsInOrder", () =>
        {
            var envelope = Soap.BuildEnvelope(Avt, "Seek", new[] { P("InstanceID", "0"), P("Unit", "REL_TIME"), P("Target", "0:01:30") });
            T.True(envelope.StartsWith("<?xml version=\"1.0\" encoding=\"utf-8\"?>"));
            var root = XmlUtils.Parse(envelope).DocumentElement;
            T.Eq("Envelope", root.LocalName);
            T.Eq("http://schemas.xmlsoap.org/soap/envelope/", root.NamespaceURI);
            T.Eq("http://schemas.xmlsoap.org/soap/encoding/", root.GetAttribute("encodingStyle", root.NamespaceURI));
            var action = XmlUtils.FindFirst(root, "Seek");
            T.Eq(Avt, action.NamespaceURI);
            T.Eq("InstanceID,Unit,Target", string.Join(",", action.ChildNodes.Cast<XmlNode>().Select(n => n.LocalName)));
            T.Eq("0:01:30", XmlUtils.ChildText(action, "Target"));
        });
        T.Run("buildsEnvelopeWithoutArguments", () =>
        {
            var root = XmlUtils.Parse(Soap.BuildEnvelope(Avt, "Stop", new KeyValuePair<string, string>[0])).DocumentElement;
            T.Eq(0, XmlUtils.FindFirst(root, "Stop").ChildNodes.Count);
        });
        T.Run("buildsSoapActionHeader", () => T.Eq("\"urn:schemas-upnp-org:service:AVTransport:1#Play\"", Soap.SoapActionHeader(Avt, "Play")));
        T.Run("parsesPositionInfoAnswer", () =>
        {
            var answer = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                + "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                + "<u:GetPositionInfoResponse xmlns:u=\"" + Avt + "\">"
                + "<Track>1</Track><TrackDuration>0:04:10</TrackDuration><TrackMetaData>&lt;DIDL-Lite/&gt;</TrackMetaData>"
                + "<RelTime>0:01:02</RelTime><AbsTime>NOT_IMPLEMENTED</AbsTime></u:GetPositionInfoResponse></s:Body></s:Envelope>";
            var result = Soap.ParseResponse("GetPositionInfo", 200, answer);
            T.Eq("0:04:10", result["TrackDuration"]);
            T.Eq("0:01:02", result["RelTime"]);
            T.Eq("<DIDL-Lite/>", result["TrackMetaData"]);
        });
        T.Run("parsesUpnpError", () =>
        {
            var fault = "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                + "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
                + "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>714</errorCode><errorDescription>Illegal MIME-type</errorDescription>"
                + "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
            try { Soap.ParseResponse("SetAVTransportURI", 500, fault); throw new Exception("Expected an exception"); }
            catch (UpnpException e)
            {
                T.Eq(714, e.ErrorCode);
                T.Eq("Illegal MIME-type", e.ErrorDescription);
                T.Eq("SetAVTransportURI", e.Action);
            }
        });
        T.Run("httpErrorWithoutBodyIsReported", () =>
        {
            try { Soap.ParseResponse("Play", 503, ""); throw new Exception("Expected an exception"); }
            catch (UpnpException e) { T.Eq(503, e.ErrorCode); }
        });
        T.Run("parsesTransportStates", () =>
        {
            T.Eq(TransportState.Playing, TransportStates.Parse("PLAYING"));
            T.Eq(TransportState.Paused, TransportStates.Parse("PAUSED_PLAYBACK"));
            T.Eq(TransportState.Stopped, TransportStates.Parse(" stopped "));
            T.Eq(TransportState.NoMedia, TransportStates.Parse("NO_MEDIA_PRESENT"));
            T.Eq(TransportState.Unknown, TransportStates.Parse("CUSTOM"));
            T.Eq(TransportState.Unknown, TransportStates.Parse(null));
        });
    }

    public static void LanHttpTest()
    {
        Console.WriteLine("LanHttpTest");
        LanHttpResponse Read(string raw, bool isHead = false) => LanHttp.ReadResponse(new MemoryStream(Encoding.UTF8.GetBytes(raw)), isHead);
        T.Run("readsBodyWithContentLength", () =>
        {
            var r = Read("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: b\r\n\r\nhelloEXTRA");
            T.Eq(200, r.Code);
            T.Eq("hello", r.BodyAsString());
            T.Eq("b", r.Headers["x-a"]);
        });
        T.Run("readsChunkedBody", () =>
            T.Eq("<root>é</root>", Read("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n<roo\r\nA;ext=1\r\nt>é</root\r\n1\r\n>\r\n0\r\nTrailer: x\r\n\r\n").BodyAsString()));
        T.Run("readsBodyUntilConnectionCloses", () => T.Eq("all of it", Read("HTTP/1.0 200 OK\r\n\r\nall of it").BodyAsString()));
        T.Run("headAnswerHasNoBody", () => T.Eq(0, Read("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", true).Body.Length));
        T.Run("rejectsMalformedAnswers", () =>
        {
            foreach (var raw in new[] { "", "garbage\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort" })
                T.Throws<IOException>(() => Read(raw), raw);
        });
        T.Run("onlyLocalAddressesAreAllowed", () =>
        {
            foreach (var a in new[] { "192.168.1.10", "10.1.2.3", "172.16.0.1", "169.254.1.1", "127.0.0.1", "fd12:3456::1", "fe80::1" })
                T.True(LanHttp.IsLocalAddress(IPAddress.Parse(a)), a);
            foreach (var a in new[] { "8.8.8.8", "2001:4860::8888" })
                T.True(!LanHttp.IsLocalAddress(IPAddress.Parse(a)), a);
        });
        T.Run("refusesPublicAndHttpsUrls", () =>
        {
            foreach (var url in new[] { "http://8.8.8.8/desc.xml", "https://192.168.1.1/desc.xml", "not a url" })
                T.Throws<IOException>(() => LanHttp.Request(url), url);
        });
    }

    public static void CastLogTest()
    {
        Console.WriteLine("CastLogTest");
        var sink = CastLog.Sink;
        CastLog.Sink = null;
        T.Run("keepsOnlyTheLatestEntries", () =>
        {
            CastLog.Clear();
            for (int i = 0; i < CastLog.MaxEntries + 10; i++) CastLog.D("line " + i);
            T.Eq(CastLog.MaxEntries, CastLog.Size());
            var dump = CastLog.Dump();
            T.True(!dump.Contains("line 9\n"));
            T.True(dump.Contains("line 10\n"));
            T.True(dump.EndsWith("line " + (CastLog.MaxEntries + 9)));
        });
        T.Run("truncatesLongEntriesAndIncludesErrors", () =>
        {
            CastLog.Clear();
            CastLog.D(new string('x', CastLog.MaxEntryLength + 5));
            CastLog.E("failed", new InvalidOperationException("boom"));
            var dump = CastLog.Dump();
            T.True(dump.Contains("[5 more chars]"));
            T.True(dump.Contains("E failed"));
            // IllegalStateException in the Kotlin test
            T.True(dump.Contains("InvalidOperationException: boom"));
        });
        T.Run("dumpRecentKeepsTheNewestEntries", () =>
        {
            CastLog.Clear();
            for (int i = 0; i < 50; i++) CastLog.D("entry " + i);
            var recent = CastLog.DumpRecent(200);
            T.True(recent.Length <= 200);
            T.True(recent.EndsWith("entry 49"));
            T.True(!recent.Contains("entry 0\n"));
        });
        CastLog.Clear();
        CastLog.Sink = sink;
    }

    static CastMedia Media() => new CastMedia
    {
        Url = "http://192.168.1.20:41234/v/abc.mp4",
        Title = "Tom & Jerry <Live> \"Best\" 'of' 🎬🔥",
        Uploader = "Ben & Co",
        MimeType = "video/mp4",
        DurationSeconds = 3725
    };

    static CastMedia With(this CastMedia m, Action<CastMedia> change)
    {
        var copy = (CastMedia)typeof(CastMedia).GetMethod("MemberwiseClone", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).Invoke(m, null);
        change(copy);
        return copy;
    }

    public static void DidlLiteTest()
    {
        Console.WriteLine("DidlLiteTest");
        var media = Media();
        T.Run("escapesSpecialCharacters", () => T.Eq("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;", XmlUtils.Escape("a & b <c> \"d\" 'e'")));
        T.Run("keepsEmojisAndRemovesInvalidCharacters", () =>
        {
            T.Eq("ok 🎬 ok", XmlUtils.Escape("ok 🎬 ok"));
            T.Eq("ab", XmlUtils.Escape("a\u0000\u0008b"));
            T.Eq("tab\tnew\nline", XmlUtils.Escape("tab\tnew\nline"));
            T.Eq("ab", XmlUtils.Escape("a\uD83Cb"));
            T.Eq("ab", XmlUtils.Escape("a\uDFAC" + "b"));
        });
        T.Run("buildsValidDidlWithAllFields", () =>
        {
            var root = XmlUtils.Parse(DidlLite.Build(media)).DocumentElement;
            T.Eq("DIDL-Lite", root.LocalName);
            var item = XmlUtils.Child(root, "item");
            T.Eq(media.Title, XmlUtils.ChildText(item, "title"));
            T.Eq("Ben & Co", XmlUtils.ChildText(item, "creator"));
            T.Eq("Ben & Co", XmlUtils.ChildText(item, "artist"));
            T.Eq("object.item.videoItem", XmlUtils.ChildText(item, "class"));
            var res = XmlUtils.Child(item, "res");
            T.Eq("http-get:*:video/mp4:*", res.GetAttribute("protocolInfo"));
            T.Eq("1:02:05.000", res.GetAttribute("duration"));
            T.Eq(media.Url, res.InnerText);
        });
        T.Run("liveStreamCarriesItsDlnaFeatures", () =>
        {
            var format = new TsFormat("video/mpeg", "AVC_TS_HD_EU_ISO");
            var live = media.With(m => { m.MimeType = format.MimeType; m.DlnaFeatures = format.DlnaFeatures; });
            var res = XmlUtils.Child(XmlUtils.Child(XmlUtils.Parse(DidlLite.Build(live)).DocumentElement, "item"), "res");
            T.Eq("http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_HD_EU_ISO;" + TsFormat.LiveFeatures, res.GetAttribute("protocolInfo"));
        });
        T.Run("rawTextNeverAppearsUnescaped", () =>
        {
            var didl = DidlLite.Build(media);
            T.True(!didl.Contains("Tom & Jerry"));
            T.True(didl.Contains("Tom &amp; Jerry &lt;Live&gt; &quot;Best&quot; &apos;of&apos; 🎬🔥"));
        });
        T.Run("durationIsOmittedWhenUnknown", () =>
        {
            var res = XmlUtils.FindFirst(XmlUtils.Parse(DidlLite.Build(media.With(m => m.DurationSeconds = -1))).DocumentElement, "res");
            T.True(!res.HasAttribute("duration"));
        });
        T.Run("urlWithQueryIsEscaped", () =>
        {
            var didl = DidlLite.Build(media.With(m => m.Url = "https://h/videoplayback?a=1&b=2"));
            T.True(didl.Contains("https://h/videoplayback?a=1&amp;b=2"));
            T.Eq("https://h/videoplayback?a=1&b=2", XmlUtils.FindFirst(XmlUtils.Parse(didl).DocumentElement, "res").InnerText);
        });
        T.Run("didlSurvivesBeingPutInsideSoapEnvelope", () =>
        {
            var didl = DidlLite.Build(media);
            var envelope = Soap.BuildEnvelope(Avt, "SetAVTransportURI", new[] { P("InstanceID", "0"), P("CurrentURI", media.Url), P("CurrentURIMetaData", didl) });
            // the metadata is escaped a second time inside the envelope
            T.True(envelope.Contains("&lt;DIDL-Lite"));
            T.True(envelope.Contains("Tom &amp;amp; Jerry"));
            var metadata = XmlUtils.FindFirst(XmlUtils.Parse(envelope).DocumentElement, "CurrentURIMetaData");
            T.Eq(didl, metadata.InnerText);
            T.Eq(media.Title, XmlUtils.FindFirst(XmlUtils.Parse(metadata.InnerText).DocumentElement, "title").InnerText);
        });
        T.Run("formatsAndParsesUpnpTime", () =>
        {
            T.Eq("0:00:00", UpnpTime.Format(0));
            T.Eq("0:03:25", UpnpTime.Format(205));
            T.Eq("10:00:01", UpnpTime.Format(36001));
            T.Eq("0:00:00", UpnpTime.Format(-5));
            T.Eq(205L, UpnpTime.Parse("0:03:25"));
            T.Eq(205L, UpnpTime.Parse("00:03:25.500"));
            T.Eq(3725L, UpnpTime.Parse("1:02:05.1/3"));
            T.Eq(-1L, UpnpTime.Parse("NOT_IMPLEMENTED"));
            T.Eq(-1L, UpnpTime.Parse(""));
            T.Eq(-1L, UpnpTime.Parse(null));
        });
        T.Run("announcesSubtitles", () =>
        {
            var withSubtitles = media.With(m => m.SubtitleUrl = "http://192.168.1.20:41234/v/def.srt");
            var item = XmlUtils.Child(XmlUtils.Parse(DidlLite.Build(withSubtitles)).DocumentElement, "item");
            T.Eq(withSubtitles.SubtitleUrl, XmlUtils.ChildText(item, "CaptionInfoEx"));
            T.Eq("srt", XmlUtils.Child(item, "CaptionInfoEx").GetAttribute("type", "http://www.sec.co.kr/"));
            var res = XmlUtils.Children(item, "res");
            T.Eq(2, res.Count);
            T.Eq(media.Url, res[0].InnerText);
            T.Eq("http-get:*:text/srt:*", res[1].GetAttribute("protocolInfo"));
            T.True(!DidlLite.Build(media).Contains("CaptionInfo"));
        });
    }

    public static void SrtConverterTest()
    {
        Console.WriteLine("SrtConverterTest");
        T.Run("convertsTtml", () =>
        {
            var ttml = @"<?xml version=""1.0"" encoding=""utf-8"" ?>
            <tt xml:lang=""en"" xmlns=""http://www.w3.org/ns/ttml"" xmlns:tts=""http://www.w3.org/ns/ttml#styling"">
            <head><styling><style xml:id=""s1"" tts:color=""white""/></styling></head>
            <body><div>
            <p begin=""00:00:01.080"" end=""00:00:03.320"" style=""s1"">Hello &amp; <span tts:fontStyle=""italic"">welcome</span></p>
            <p begin=""00:00:03.320"" end=""00:00:05.000"">first line<br/>second line</p>
            <p begin=""00:01:02.000"" dur=""1.5s"">later</p>
            <p begin=""00:01:05.000"" end=""00:01:06.000""> </p>
            </div></body></tt>";
            T.Eq("1\n00:00:01,080 --> 00:00:03,320\nHello & welcome\n\n"
                + "2\n00:00:03,320 --> 00:00:05,000\nfirst line\nsecond line\n\n"
                + "3\n00:01:02,000 --> 00:01:03,500\nlater\n\n", SrtConverter.FromTtml(ttml));
        });
        T.Run("convertsTtmlTicks", () =>
        {
            var ttml = @"<tt xmlns=""http://www.w3.org/ns/ttml"" xmlns:ttp=""http://www.w3.org/ns/ttml#parameter"" ttp:tickRate=""10000000"">
            <body><div><p begin=""15000000t"" end=""30000000t"">ticks</p></div></body></tt>";
            T.Eq("1\n00:00:01,500 --> 00:00:03,000\nticks\n\n", SrtConverter.FromTtml(ttml));
        });
        T.Run("convertsVtt", () =>
        {
            var vtt = "WEBVTT\nKind: captions\nLanguage: en\n\n"
                + "00:00:00.500 --> 00:00:02.000 align:start position:0%\n<c>Hi</c> <00:00:01.000><c>there</c>\n\n"
                + "note\n01:02.000 --> 01:03.250\nshort &amp; sweet\n";
            T.Eq("1\n00:00:00,500 --> 00:00:02,000\nHi there\n\n2\n00:01:02,000 --> 00:01:03,250\nshort & sweet\n\n", SrtConverter.FromVtt(vtt));
        });
        T.Run("shiftsForVideosStartedInTheMiddle", () =>
        {
            var srt = "1\n00:00:01,000 --> 00:00:02,000\ngone\n\n2\n00:00:09,000 --> 00:00:12,000\ncut\n\n3\n00:01:00,000 --> 00:01:01,000\nlater\n\n";
            T.Eq("1\n00:00:00,000 --> 00:00:02,000\ncut\n\n2\n00:00:50,000 --> 00:00:51,000\nlater\n\n", SrtConverter.Shift(srt, 10000));
        });
        T.Run("supportedFormats", () =>
        {
            T.True(SrtConverter.IsSupported("ttml"));
            T.True(SrtConverter.IsSupported("VTT"));
            T.True(!SrtConverter.IsSupported("srv3"));
            T.True(!SrtConverter.IsSupported(null));
            T.Eq(null, SrtConverter.Convert("x", "json3"));
            T.Eq("x", SrtConverter.Convert("x", "srt"));
        });
        T.Run("formatsTime", () => T.Eq("01:02:03,004", SrtConverter.FormatTime(3723004)));
    }

    static string Description(string controlAvt, string controlRc = "/upnp/control/RenderingControl1", string urlBase = null, string friendlyName = "[TV] Samsung 6 Series (43)")
    {
        var @base = urlBase != null ? "<URLBase>" + urlBase + "</URLBase>" : "";
        var rc = controlRc != null
            ? "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
              + "<controlURL>" + controlRc + "</controlURL><eventSubURL>/upnp/event/RenderingControl1</eventSubURL><SCPDURL>/RenderingControl_1.xml</SCPDURL></service>"
            : "";
        return "<?xml version=\"1.0\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:sec=\"http://www.sec.co.kr/dlna\">"
            + "<specVersion><major>1</major><minor>0</minor></specVersion>" + @base
            + "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType><friendlyName>" + friendlyName + "</friendlyName>"
            + "<manufacturer>Samsung Electronics</manufacturer><modelName>UE43TU7000</modelName><UDN>uuid:0a1b2c3d-0000-1000-8000-aabbccddeeff</UDN>"
            + "<sec:ProductCap>Tab,Streaming,Y2020</sec:ProductCap><serviceList>" + rc
            + "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/upnp/control/ConnectionManager1</controlURL></service>"
            + "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
            + "<controlURL>" + controlAvt + "</controlURL></service></serviceList></device></root>";
    }

    public static void DeviceDescriptionParserTest()
    {
        Console.WriteLine("DeviceDescriptionParserTest");
        T.Run("parsesSamsungDescriptionWithAbsolutePaths", () =>
        {
            var d = DeviceDescriptionParser.Parse(Description("/upnp/control/AVTransport1"), "http://192.168.1.50:9197/dmr");
            T.Eq("[TV] Samsung 6 Series (43)", d.FriendlyName);
            T.Eq("uuid:0a1b2c3d-0000-1000-8000-aabbccddeeff", d.Udn);
            T.Eq("Samsung Electronics", d.Manufacturer);
            T.Eq("UE43TU7000", d.ModelName);
            T.Eq("http://192.168.1.50:9197/upnp/control/AVTransport1", d.AvTransportControlUrl);
            T.Eq("urn:schemas-upnp-org:service:AVTransport:1", d.AvTransportServiceType);
            T.Eq("http://192.168.1.50:9197/upnp/control/RenderingControl1", d.RenderingControlUrl);
            T.Eq("urn:schemas-upnp-org:service:RenderingControl:1", d.RenderingControlServiceType);
            T.Eq("http://192.168.1.50:9197/upnp/control/ConnectionManager1", d.ConnectionManagerUrl);
            T.Eq("urn:schemas-upnp-org:service:ConnectionManager:1", d.ConnectionManagerServiceType);
        });
        T.Run("labelShowsTheBrandWhenItIsNotInTheName", () =>
        {
            DlnaDevice Dev(string name, string manufacturer) => new DlnaDevice { Udn = "uuid:1", FriendlyName = name, Manufacturer = manufacturer, Location = "http://tv/", AvTransportControlUrl = "http://tv/av", AvTransportServiceType = "av" };
            T.Eq("Tele (Samsung)", Dev("Tele", "Samsung Electronics").Label);
            T.Eq("[LG] webOS TV TN510S", Dev("[LG] webOS TV TN510S", "LG Electronics.").Label);
            T.Eq("XBOXONE (Microsoft)", Dev("XBOXONE", "Microsoft Corporation").Label);
            T.Eq("Tele", Dev("Tele", null).Label);
        });
        T.Run("resolvesRelativeUrlsAgainstLocation", () =>
        {
            var d = DeviceDescriptionParser.Parse(Description("AVTransport/control", "RenderingControl/control"), "http://10.0.0.7:49152/dev/desc.xml");
            T.Eq("http://10.0.0.7:49152/dev/AVTransport/control", d.AvTransportControlUrl);
            T.Eq("http://10.0.0.7:49152/dev/RenderingControl/control", d.RenderingControlUrl);
        });
        T.Run("urlBaseHasPriorityOverLocation", () =>
        {
            var d = DeviceDescriptionParser.Parse(Description("upnp/control/avt", urlBase: "http://10.0.0.8:8080/"), "http://10.0.0.7:49152/dev/desc.xml");
            T.Eq("http://10.0.0.8:8080/upnp/control/avt", d.AvTransportControlUrl);
            T.Eq("http://10.0.0.8:8080/upnp/control/RenderingControl1", d.RenderingControlUrl);
        });
        T.Run("urlBaseWithoutPathIsTheRoot", () =>
            T.Eq("http://10.0.0.8:8080/control/avt", DeviceDescriptionParser.Parse(Description("control/avt", urlBase: "http://10.0.0.8:8080"), "http://10.0.0.7:49152/dev/desc.xml").AvTransportControlUrl));
        T.Run("absoluteControlUrlIsKept", () =>
        {
            var d = DeviceDescriptionParser.Parse(Description("http://10.0.0.9:1234/AVT", controlRc: null), "http://10.0.0.7:49152/desc.xml");
            T.Eq("http://10.0.0.9:1234/AVT", d.AvTransportControlUrl);
            T.Eq(null, d.RenderingControlUrl);
            T.Eq(null, d.RenderingControlServiceType);
        });
        T.Run("findsRendererInEmbeddedDevice", () =>
        {
            var xml = "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device><friendlyName>NAS</friendlyName><UDN>uuid:root</UDN>"
                + "<serviceList><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType><controlURL>/cd</controlURL></service></serviceList>"
                + "<deviceList><device><friendlyName>Living room</friendlyName><UDN>uuid:renderer</UDN>"
                + "<serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:2</serviceType><controlURL>/avt</controlURL></service></serviceList>"
                + "</device></deviceList></device></root>";
            var d = DeviceDescriptionParser.Parse(xml, "http://10.0.0.3:80/desc.xml");
            T.Eq("Living room", d.FriendlyName);
            T.Eq("uuid:renderer", d.Udn);
            // java.net.URI keeps the explicit default port :80, System.Uri drops it: same URL for the TV
            T.True(d.AvTransportControlUrl == "http://10.0.0.3:80/avt" || d.AvTransportControlUrl == "http://10.0.0.3/avt", d.AvTransportControlUrl);
            T.Eq("urn:schemas-upnp-org:service:AVTransport:2", d.AvTransportServiceType);
        });
        T.Run("deviceWithoutAvTransportIsIgnored", () =>
        {
            var xml = "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device><friendlyName>Router</friendlyName>"
                + "<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl</controlURL></service></serviceList></device></root>";
            T.Eq(null, DeviceDescriptionParser.Parse(xml, "http://10.0.0.1/desc.xml"));
        });
        T.Run("missingFriendlyNameFallsBackToHost", () =>
            T.Eq("10.0.0.4", DeviceDescriptionParser.Parse(Description("/avt", friendlyName: ""), "http://10.0.0.4:9197/dmr").FriendlyName));
        T.Run("descriptionWithBomAndEscapedNameIsParsed", () =>
            T.Eq("Tom & Jerry's TV", DeviceDescriptionParser.Parse("﻿" + Description("/avt", friendlyName: "Tom &amp; Jerry&apos;s TV"), "http://10.0.0.4:9197/dmr").FriendlyName));
        T.Run("resolveUrlFollowsRfc3986", () =>
        {
            var b = "http://1.2.3.4:5/a/b/desc.xml";
            T.Eq("http://1.2.3.4:5/x", DeviceDescriptionParser.ResolveUrl(b, "/x"));
            T.Eq("http://1.2.3.4:5/a/b/x", DeviceDescriptionParser.ResolveUrl(b, "x"));
            T.Eq("http://1.2.3.4:5/a/x", DeviceDescriptionParser.ResolveUrl(b, "../x"));
            T.Eq("https://h/x", DeviceDescriptionParser.ResolveUrl(b, " https://h/x "));
        });
    }

    static CastStreamCandidate Stream(int height, string mime = "video/mp4", bool videoOnly = false, bool progressive = true) =>
        new CastStreamCandidate { Url = "https://h/" + height + "-" + mime + "-" + videoOnly + "-" + progressive, MimeType = mime, Height = height, IsVideoOnly = videoOnly, IsProgressiveHttp = progressive };

    static CastStreamCandidate VideoOnly(int height, string codec = "avc1.640028", bool ranges = true, int bitrate = 1000) => new CastStreamCandidate
    {
        Url = "https://h/v" + height + "-" + codec, MimeType = "video/mp4", Height = height, IsVideoOnly = true, IsProgressiveHttp = true,
        Codec = codec, Width = height * 16 / 9, Bitrate = bitrate,
        InitRange = ranges ? new LongRange(0, 740) : null, IndexRange = ranges ? new LongRange(741, 1500) : null
    };

    static CastAudioCandidate Audio(string codec = "mp4a.40.2", int bitrate = 128000, bool original = true, string mime = "audio/mp4", string track = null) => new CastAudioCandidate
    {
        Url = "https://h/a-" + codec + "-" + bitrate + "-" + original.ToString().ToLowerInvariant() + (track != null ? "-" + track : ""),
        MimeType = mime, IsProgressiveHttp = true, Codec = codec, Bitrate = bitrate, IsOriginal = original,
        InitRange = new LongRange(0, 630), IndexRange = new LongRange(631, 900), TrackId = track
    };

    public static void CastStreamSelectorTest()
    {
        Console.WriteLine("CastStreamSelectorTest");
        T.Run("choosesHighestProgressiveMp4UpTo720p", () =>
            T.Eq(720, CastStreamSelector.Select(new[] { Stream(360), Stream(720), Stream(480), Stream(1080, videoOnly: true) }).Height));
        T.Run("ignoresVideoOnlyNonMp4AndNonProgressive", () =>
            T.Eq(240, CastStreamSelector.Select(new[] { Stream(720, videoOnly: true), Stream(720, mime: "video/webm"), Stream(720, progressive: false), Stream(240) }).Height));
        T.Run("ignoresResolutionsAbove720p", () => T.Eq(null, CastStreamSelector.Select(new[] { Stream(1080), Stream(1440) })));
        T.Run("returnsNullWithoutUsableStreams", () =>
        {
            T.Eq(null, CastStreamSelector.Select(new CastStreamCandidate[0]));
            T.Eq(null, CastStreamSelector.Select(new[] { Stream(360, mime: null) }));
        });
        T.Run("dashPrefersH264UpTo1080p", () =>
        {
            var dash = CastStreamSelector.SelectDash(new[] { VideoOnly(720), VideoOnly(1080), VideoOnly(1440), VideoOnly(2160, "av01.0.12M.08") }, new[] { Audio() });
            T.Eq(1080, dash.Video.Height);
            T.Eq("avc1.640028", dash.Video.Codecs);
            T.Eq(0L, dash.Video.InitStart);
            T.Eq(1500L, dash.Video.IndexEnd);
        });
        T.Run("dashNeedsH264AndAac", () =>
        {
            T.Eq(null, CastStreamSelector.SelectDash(new[] { VideoOnly(1080, "av01.0.08M.08"), VideoOnly(720, "vp09.00.40.08") }, new[] { Audio() }));
            T.Eq(null, CastStreamSelector.SelectDash(new[] { VideoOnly(720) }, new[] { Audio(codec: "opus") }));
        });
        T.Run("dashPrefersOriginalAacAudio", () =>
        {
            var dash = CastStreamSelector.SelectDash(new[] { VideoOnly(720) }, new[]
            {
                Audio(bitrate: 256000, original: false), Audio(codec: "opus", bitrate: 160000, mime: "audio/webm"),
                Audio(codec: "ac-3", bitrate: 384000), Audio(bitrate: 48000), Audio(bitrate: 128000)
            });
            T.Eq("https://h/a-mp4a.40.2-128000-true", dash.Audio.Url);
        });
        T.Run("dashNeedsRangesAndAudio", () =>
        {
            T.Eq(null, CastStreamSelector.SelectDash(new[] { VideoOnly(720, ranges: false) }, new[] { Audio() }));
            T.Eq(null, CastStreamSelector.SelectDash(new[] { VideoOnly(720) }, new CastAudioCandidate[0]));
            T.Eq(null, CastStreamSelector.SelectDash(new CastStreamCandidate[0], new[] { Audio() }));
        });
        T.Run("rangeOfRejectsUnknownRanges", () =>
        {
            T.Eq(null, CastStreamSelector.RangeOf(-1, -1));
            T.Eq(null, CastStreamSelector.RangeOf(0, 0));
            var r = CastStreamSelector.RangeOf(0, 740);
            T.True(r.First == 0 && r.Last == 740);
        });
        T.Run("listsQualitiesOfProgressiveAndMuxedStreams", () =>
            T.Eq("1080,720,480,360", string.Join(",", CastStreamSelector.Qualities(new[] { Stream(360), Stream(720), Stream(1080) },
                new[] { VideoOnly(1080), VideoOnly(720), VideoOnly(480), VideoOnly(1440), VideoOnly(720, "vp09.00.40.08") }))));
        T.Run("autoQualityPrefersProgressive", () =>
        {
            var choice = CastStreamSelector.Choose(new[] { Stream(360) }, new[] { VideoOnly(1080) }, new[] { Audio() });
            T.Eq(360, choice.Single.Height);
            T.Eq(null, choice.Dash);
        });
        T.Run("chosenQualityUsesMuxedStreamsWhenNeeded", () =>
        {
            var progressive = new[] { Stream(360) };
            var videos = new[] { VideoOnly(480), VideoOnly(720), VideoOnly(1080) };
            T.Eq(360, CastStreamSelector.Choose(progressive, videos, new[] { Audio() }, height: 360).Single.Height);
            T.Eq(720, CastStreamSelector.Choose(progressive, videos, new[] { Audio() }, height: 720).Dash.Video.Height);
        });
        T.Run("otherAudioTrackIsAlwaysMuxed", () =>
        {
            var audios = new[] { Audio(track: "en.4"), Audio(original: false, track: "it.3") };
            T.Eq("en.4,it.3", string.Join(",", CastStreamSelector.AudioTracks(audios)));
            var dubbed = CastStreamSelector.Choose(new[] { Stream(360) }, new[] { VideoOnly(720) }, audios, audioTrackId: "it.3");
            T.Eq("https://h/a-mp4a.40.2-128000-false-it.3", dubbed.Dash.Audio.Url);
            var original = CastStreamSelector.Choose(new[] { Stream(360) }, new[] { VideoOnly(720) }, audios, audioTrackId: "en.4");
            T.Eq(360, original.Single.Height);
        });
        T.Run("noAudioTrackChoiceWithOneTrack", () =>
        {
            T.Eq(0, CastStreamSelector.AudioTracks(new[] { Audio(), Audio(bitrate: 48000) }).Count);
            T.Eq(0, CastStreamSelector.AudioTracks(new[] { Audio(track: "en"), Audio(track: "en") }).Count);
        });
    }

    // --- DlnaClientTest: a fake renderer which records the SOAP requests ---

    sealed class Received
    {
        public string Path, SoapAction, ContentType, Body;
    }

    sealed class FakeRenderer : IDisposable
    {
        readonly TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
        public readonly List<Received> ReceivedRequests = new List<Received>();
        public readonly Dictionary<string, KeyValuePair<int, string>> Answers = new Dictionary<string, KeyValuePair<int, string>>();
        public int Port => ((IPEndPoint)listener.LocalEndpoint).Port;

        public FakeRenderer()
        {
            listener.Start();
            Task.Run(Loop);
        }

        async Task Loop()
        {
            while (true)
            {
                TcpClient client;
                try { client = await listener.AcceptTcpClientAsync(); } catch (Exception) { return; }
                using (client)
                {
                    var stream = client.GetStream();
                    var request = Perle.Cast.Proxy.HttpRequest.Read(stream);
                    int length = int.Parse(request.Header("Content-Length") ?? "0");
                    var body = new byte[length];
                    int read = 0;
                    while (read < length) read += stream.Read(body, read, length - read);
                    var soapAction = request.Header("SOAPACTION");
                    lock (ReceivedRequests) ReceivedRequests.Add(new Received { Path = request.Path, SoapAction = soapAction, ContentType = request.Header("Content-Type"), Body = Encoding.UTF8.GetString(body) });
                    var action = soapAction == null ? "" : soapAction.Substring(soapAction.IndexOf('#') + 1).TrimEnd('"');
                    var answer = Answers.TryGetValue(action, out var a) ? a : new KeyValuePair<int, string>(200, EmptyAnswer(action));
                    var bytes = Encoding.UTF8.GetBytes(answer.Value);
                    var head = Encoding.ASCII.GetBytes("HTTP/1.1 " + answer.Key + " X\r\nContent-Length: " + bytes.Length + "\r\n\r\n");
                    stream.Write(head, 0, head.Length);
                    stream.Write(bytes, 0, bytes.Length);
                }
            }
        }

        public void Dispose() => listener.Stop();
    }

    static string EmptyAnswer(string action, string content = "", string service = Avt) =>
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        + "<s:Body><u:" + action + "Response xmlns:u=\"" + service + "\">" + content + "</u:" + action + "Response></s:Body></s:Envelope>";

    static void WithRenderer(Action<FakeRenderer, DlnaClient> test)
    {
        using (var server = new FakeRenderer())
        {
            var b = "http://127.0.0.1:" + server.Port;
            var client = new DlnaClient(new DlnaDevice
            {
                Udn = "uuid:test", FriendlyName = "Fake TV", Location = b + "/dmr",
                AvTransportControlUrl = b + "/upnp/control/AVTransport1", AvTransportServiceType = Avt,
                RenderingControlUrl = b + "/upnp/control/RenderingControl1", RenderingControlServiceType = Rc
            });
            test(server, client);
        }
    }

    public static void DlnaClientTest()
    {
        Console.WriteLine("DlnaClientTest");
        T.Run("setAvTransportUriSendsMetadata", () => WithRenderer((server, client) =>
        {
            var media = new CastMedia { Url = "http://192.168.1.2:1234/v/a.mp4", Title = "A & B 🎵", Uploader = "Me", MimeType = "video/mp4", DurationSeconds = 61 };
            client.SetAvTransportUri(media);
            var request = server.ReceivedRequests.Single();
            T.Eq("/upnp/control/AVTransport1", request.Path);
            T.Eq("\"" + Avt + "#SetAVTransportURI\"", request.SoapAction);
            T.True(request.ContentType.StartsWith("text/xml"));
            var action = XmlUtils.FindFirst(XmlUtils.Parse(request.Body).DocumentElement, "SetAVTransportURI");
            T.Eq("0", XmlUtils.ChildText(action, "InstanceID"));
            T.Eq(media.Url, XmlUtils.ChildText(action, "CurrentURI"));
            T.Eq(DidlLite.Build(media), XmlUtils.ChildText(action, "CurrentURIMetaData"));
        }));
        T.Run("controlActionsUseTheRightArguments", () => WithRenderer((server, client) =>
        {
            client.Play();
            client.Pause();
            client.Seek(3725);
            client.Stop();
            var r = server.ReceivedRequests;
            T.Eq("Play,Pause,Seek,Stop", string.Join(",", r.Select(x => x.SoapAction.Substring(x.SoapAction.IndexOf('#') + 1).TrimEnd('"'))));
            T.Eq("1", XmlUtils.ChildText(XmlUtils.FindFirst(XmlUtils.Parse(r[0].Body).DocumentElement, "Play"), "Speed"));
            var seek = XmlUtils.FindFirst(XmlUtils.Parse(r[2].Body).DocumentElement, "Seek");
            T.Eq("REL_TIME", XmlUtils.ChildText(seek, "Unit"));
            T.Eq("1:02:05", XmlUtils.ChildText(seek, "Target"));
        }));
        T.Run("readsPositionAndTransportState", () => WithRenderer((server, client) =>
        {
            server.Answers["GetPositionInfo"] = new KeyValuePair<int, string>(200, EmptyAnswer("GetPositionInfo", "<Track>1</Track><TrackDuration>0:10:00</TrackDuration><RelTime>0:00:42</RelTime>"));
            server.Answers["GetTransportInfo"] = new KeyValuePair<int, string>(200, EmptyAnswer("GetTransportInfo",
                "<CurrentTransportState>PAUSED_PLAYBACK</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>"));
            var position = client.GetPositionInfo();
            T.True(position.PositionSeconds == 42 && position.DurationSeconds == 600, "position 42/600");
            T.Eq(TransportState.Paused, client.GetTransportInfo());
        }));
        T.Run("volumeUsesRenderingControl", () => WithRenderer((server, client) =>
        {
            server.Answers["GetVolume"] = new KeyValuePair<int, string>(200, EmptyAnswer("GetVolume", "<CurrentVolume>17</CurrentVolume>", Rc));
            T.True(client.HasVolumeControl);
            T.Eq(17, client.GetVolume());
            client.SetVolume(18);
            var r = server.ReceivedRequests;
            T.Eq("/upnp/control/RenderingControl1", r[1].Path);
            T.Eq("\"" + Rc + "#SetVolume\"", r[1].SoapAction);
            var setVolume = XmlUtils.FindFirst(XmlUtils.Parse(r[1].Body).DocumentElement, "SetVolume");
            T.Eq("Master", XmlUtils.ChildText(setVolume, "Channel"));
            T.Eq("18", XmlUtils.ChildText(setVolume, "DesiredVolume"));
        }));
        T.Run("upnpErrorsAreThrownAndLogged", () => WithRenderer((server, client) =>
        {
            CastLog.Clear();
            server.Answers["Play"] = new KeyValuePair<int, string>(500, "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                + "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
                + "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>701</errorCode>"
                + "<errorDescription>Transition not available</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
            try { client.Play(); throw new Exception("Expected an exception"); }
            catch (UpnpException e) { T.Eq(701, e.ErrorCode); }
            T.True(CastLog.Dump().Contains("UPnP error 701"));
        }));
    }

    sealed class FakeHost : ICastHost
    {
        public readonly List<string> Events = new List<string>();
        public long SavedPositionSeconds(CastItem item) { lock (Events) Events.Add("saved?"); return 10; }
        public void SaveProgress(CastItem item, long positionSeconds) { lock (Events) Events.Add("save " + positionSeconds); }
        public void MarkAsViewed(CastItem item) { lock (Events) Events.Add("viewed " + item.Title); }
        public bool DirectLink => false;
        public void SessionStarted() { lock (Events) Events.Add("started"); }
        public void SessionEnded() { lock (Events) Events.Add("ended"); }
    }

    static bool WaitFor(Func<bool> condition, int millis = 10000)
    {
        var until = DateTime.UtcNow.AddMilliseconds(millis);
        while (DateTime.UtcNow < until) { if (condition()) return true; System.Threading.Thread.Sleep(50); }
        return condition();
    }

    public static void CastServiceTest()
    {
        Console.WriteLine("CastServiceTest");
        T.Run("sessionPlaysSeeksToSavedPositionAndStops", () =>
        {
            using (var server = new FakeRenderer())
            {
                var b = "http://127.0.0.1:" + server.Port;
                var device = new DlnaDevice
                {
                    Udn = "uuid:test", FriendlyName = "Fake TV", Location = b + "/dmr",
                    AvTransportControlUrl = b + "/upnp/control/AVTransport1", AvTransportServiceType = Avt
                };
                server.Answers["GetTransportInfo"] = new KeyValuePair<int, string>(200, EmptyAnswer("GetTransportInfo",
                    "<CurrentTransportState>PLAYING</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>"));
                server.Answers["GetPositionInfo"] = new KeyValuePair<int, string>(200, EmptyAnswer("GetPositionInfo",
                    "<Track>1</Track><TrackDuration>0:10:00</TrackDuration><RelTime>0:00:42</RelTime>"));
                var host = new FakeHost();
                var oldFinder = LocalNetwork.Finder;
                LocalNetwork.Finder = () => new LocalAddress("lo", IPAddress.Loopback);
                CastService.Host = host;
                try
                {
                    var info = new Perle.Extractor.StreamInfo { VideoId = "abc", Url = "https://www.youtube.com/watch?v=abc", Name = "Video", UploaderName = "Me", Duration = 600 };
                    info.Formats.Add(new Perle.Extractor.StreamFormat { Itag = 18, Url = "http://example.invalid/v.mp4", MimeType = "video/mp4", Codecs = "avc1.42001E, mp4a.40.2", IsVideo = true, Height = 360 });
                    var item = new CastItem(info);
                    T.True(item.IsCastable, "castable");
                    T.Eq("360", string.Join(",", item.Qualities));
                    CastService.Start(device, item);
                    T.True(CastService.IsActive, "active right away");
                    T.True(WaitFor(() => CastService.State.Status == CastStatus.Playing && CastService.State.PositionSeconds == 42), "playing at 42s, got " + CastService.State.Status + " " + CastService.State.PositionSeconds);
                    List<string> actions;
                    lock (server.ReceivedRequests) actions = server.ReceivedRequests.Select(r => r.SoapAction == null ? "" : r.SoapAction.Substring(r.SoapAction.IndexOf('#') + 1).TrimEnd('"')).ToList();
                    int set = actions.IndexOf("SetAVTransportURI");
                    T.True(set >= 0 && actions.IndexOf("Play") > set, "SetAVTransportURI then Play: " + string.Join(",", actions));
                    T.True(actions.Contains("Seek"), "seeks to the saved position");
                    Received setRequest;
                    lock (server.ReceivedRequests) setRequest = server.ReceivedRequests.First(r => r.SoapAction != null && r.SoapAction.Contains("SetAVTransportURI"));
                    var uri = XmlUtils.ChildText(XmlUtils.FindFirst(XmlUtils.Parse(setRequest.Body).DocumentElement, "SetAVTransportURI"), "CurrentURI");
                    T.True(uri.StartsWith("http://127.0.0.1:"), "proxy URL: " + uri);
                    T.Eq(600L, CastService.State.DurationSeconds);
                    CastService.Stop();
                    T.True(WaitFor(() => !CastService.IsActive), "stopped");
                    lock (host.Events)
                    {
                        T.True(host.Events.Contains("viewed Video"), "viewed");
                        T.True(host.Events.Contains("save 42"), "progress saved: " + string.Join(",", host.Events));
                        T.True(host.Events.Contains("ended"), "ended");
                    }
                }
                finally
                {
                    LocalNetwork.Finder = oldFinder;
                    CastService.Host = null;
                }
            }
        });
    }

    public static int RunAll()
    {
        SsdpTest();
        SoapTest();
        LanHttpTest();
        CastLogTest();
        DidlLiteTest();
        SrtConverterTest();
        DeviceDescriptionParserTest();
        CastStreamSelectorTest();
        DlnaClientTest();
        CastServiceTest();
        return T.Failures;
    }
}
