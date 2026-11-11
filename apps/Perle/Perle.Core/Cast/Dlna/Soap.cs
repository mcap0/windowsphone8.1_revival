/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.Soap and DlnaClient.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml;

namespace Perle.Cast.Dlna
{
    /// <summary>Thrown when a device answers a SOAP action with an error, usually a UPnPError.</summary>
    /// <param name="ErrorCode">the UPnP error code (e.g. 701 "Transition not available"), or the HTTP
    /// status code if the answer did not contain a UPnP error</param>
    public sealed class UpnpException : Exception
    {
        public string Action { get; }
        public int ErrorCode { get; }
        public string ErrorDescription { get; }

        public UpnpException(string action, int errorCode, string errorDescription)
            : base(action + " failed: " + errorCode + " " + errorDescription)
        { Action = action; ErrorCode = errorCode; ErrorDescription = errorDescription; }
    }

    /// <summary>Builds SOAP requests for UPnP actions and parses their answers.</summary>
    public static class Soap
    {
        public const string ContentType = "text/xml; charset=\"utf-8\"";

        /// <param name="arguments">the action arguments, in the order required by the service description
        /// (renderers are often strict about the order). Values are escaped here.</param>
        public static string BuildEnvelope(string serviceType, string action, IList<KeyValuePair<string, string>> arguments)
        {
            var body = new StringBuilder();
            foreach (var a in arguments) body.Append('<').Append(a.Key).Append('>').Append(XmlUtils.Escape(a.Value)).Append("</").Append(a.Key).Append('>');
            return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                + "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
                + " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                + "<s:Body>"
                + "<u:" + action + " xmlns:u=\"" + XmlUtils.Escape(serviceType) + "\">"
                + body
                + "</u:" + action + ">"
                + "</s:Body>"
                + "</s:Envelope>";
        }

        /// <returns>the value of the SOAPACTION header, quotes included as required by the spec</returns>
        public static string SoapActionHeader(string serviceType, string action) => "\"" + serviceType + "#" + action + "\"";

        /// <summary>Parses the answer to an action.</summary>
        /// <returns>the output arguments, e.g. RelTime -> 0:01:02</returns>
        /// <exception cref="UpnpException">if the answer is a SOAP fault or the HTTP request failed</exception>
        public static Dictionary<string, string> ParseResponse(string action, int httpCode, string xml)
        {
            bool ok = httpCode >= 200 && httpCode <= 299;
            XmlDocument document;
            try { document = XmlUtils.Parse(xml); }
            catch (Exception e)
            {
                if (!ok) throw new UpnpException(action, httpCode, "HTTP error without valid SOAP body");
                throw new UpnpException(action, httpCode, "Invalid SOAP answer: " + e.Message);
            }
            var root = document.DocumentElement;

            var fault = XmlUtils.FindFirst(root, "Fault");
            if (fault != null || !ok)
            {
                var upnpError = fault != null ? XmlUtils.FindFirst(fault, "UPnPError") : null;
                int code = upnpError != null && int.TryParse(XmlUtils.ChildText(upnpError, "errorCode"), out var c) ? c : httpCode;
                var description = (upnpError != null ? XmlUtils.ChildText(upnpError, "errorDescription") : null)
                    ?? (fault != null ? XmlUtils.ChildText(fault, "faultstring") : null)
                    ?? DescribeError(code);
                throw new UpnpException(action, code, description);
            }

            var response = XmlUtils.FindFirst(root, action + "Response");
            var result = new Dictionary<string, string>();
            if (response == null) return result;
            foreach (var e in response.ChildNodes.OfType<XmlElement>()) result[!string.IsNullOrEmpty(e.LocalName) ? e.LocalName : e.Name] = e.InnerText.Trim();
            return result;
        }

        /// <summary>Descriptions of common UPnP error codes, for the log and the user.</summary>
        public static string DescribeError(int code)
        {
            switch (code)
            {
                case 401: return "Invalid action";
                case 402: return "Invalid arguments";
                case 500: return "Internal server error";
                case 501: return "Action failed";
                case 701: return "Transition not available";
                case 702: return "No contents";
                case 703: return "Read error";
                case 704: return "Format not supported for playback";
                case 705: return "Transport is locked";
                case 710: return "Seek mode not supported";
                case 711: return "Illegal seek target";
                case 714: return "Illegal MIME-type";
                case 715: return "Content busy";
                case 716: return "Resource not found";
                case 718: return "Invalid InstanceID";
                default: return "Unknown error";
            }
        }
    }

    /// <summary>State of the renderer, from GetTransportInfo.</summary>
    public enum TransportState { Playing, Paused, Stopped, Transitioning, NoMedia, Unknown }

    public static class TransportStates
    {
        public static TransportState Parse(string value)
        {
            switch (value?.Trim().ToUpperInvariant())
            {
                case "PLAYING": return TransportState.Playing;
                case "PAUSED_PLAYBACK":
                case "PAUSED_RECORDING": return TransportState.Paused;
                case "STOPPED": return TransportState.Stopped;
                case "TRANSITIONING": return TransportState.Transitioning;
                case "NO_MEDIA_PRESENT": return TransportState.NoMedia;
                default: return TransportState.Unknown;
            }
        }
    }

    /// <summary>Position of the renderer, from GetPositionInfo. Values are in seconds, -1 if unknown.</summary>
    public sealed class PositionInfo
    {
        public long PositionSeconds { get; }
        public long DurationSeconds { get; }
        public PositionInfo(long positionSeconds, long durationSeconds) { PositionSeconds = positionSeconds; DurationSeconds = durationSeconds; }
    }

    /// <summary>
    /// Blocking client for the AVTransport and RenderingControl services of a DlnaDevice.
    /// Every request and answer is written to CastLog. Call the methods from a background thread.
    /// </summary>
    public sealed class DlnaClient
    {
        static readonly KeyValuePair<string, string> InstanceId = new KeyValuePair<string, string>("InstanceID", "0");
        static readonly KeyValuePair<string, string> Channel = new KeyValuePair<string, string>("Channel", "Master");
        public const string UserAgent = "Windows/10.0 UPnP/1.0 NewPipe/1.0";
        const int ReadTimeoutMillis = 10000;

        /// <summary>
        /// Samsung TVs show an "allow this device?" popup the first time and only answer after
        /// the user accepts it, so the first commands get more time.
        /// </summary>
        const int FirstContactTimeoutMillis = 30000;

        readonly DlnaDevice device;

        public DlnaClient(DlnaDevice device) { this.device = device; }

        static KeyValuePair<string, string> P(string k, string v) => new KeyValuePair<string, string>(k, v);

        public void SetAvTransportUri(CastMedia media)
        {
            var metadata = DidlLite.Build(media);
            AvTransport("SetAVTransportURI", new[] { InstanceId, P("CurrentURI", media.Url), P("CurrentURIMetaData", metadata) },
                readTimeoutMillis: FirstContactTimeoutMillis);
        }

        public void Play() => AvTransport("Play", new[] { InstanceId, P("Speed", "1") }, readTimeoutMillis: FirstContactTimeoutMillis);

        public void Pause() => AvTransport("Pause", new[] { InstanceId });

        public void Stop() => AvTransport("Stop", new[] { InstanceId });

        public void Seek(long positionSeconds) =>
            AvTransport("Seek", new[] { InstanceId, P("Unit", "REL_TIME"), P("Target", UpnpTime.Format(positionSeconds)) });

        public PositionInfo GetPositionInfo()
        {
            var result = AvTransport("GetPositionInfo", new[] { InstanceId }, logAnswer: false);
            result.TryGetValue("RelTime", out var rel);
            result.TryGetValue("TrackDuration", out var dur);
            return new PositionInfo(UpnpTime.Parse(rel), UpnpTime.Parse(dur));
        }

        public TransportState GetTransportInfo()
        {
            var result = AvTransport("GetTransportInfo", new[] { InstanceId }, logAnswer: false);
            result.TryGetValue("CurrentTransportState", out var state);
            return TransportStates.Parse(state);
        }

        /// <returns>true if the device supports volume control</returns>
        public bool HasVolumeControl => device.RenderingControlUrl != null && device.RenderingControlServiceType != null;

        /// <returns>the volume, usually 0..100</returns>
        public int GetVolume()
        {
            var result = RenderingControl("GetVolume", new[] { InstanceId, Channel });
            if (result.TryGetValue("CurrentVolume", out var v) && int.TryParse(v, out var volume)) return volume;
            throw new UpnpException("GetVolume", 0, "No CurrentVolume in answer");
        }

        public void SetVolume(int volume) => RenderingControl("SetVolume", new[] { InstanceId, Channel, P("DesiredVolume", volume.ToString()) });

        /// <returns>the formats the TV can play (the Sink of GetProtocolInfo), or null if the
        /// device has no ConnectionManager service</returns>
        public string GetSinkProtocolInfo()
        {
            if (device.ConnectionManagerUrl == null || device.ConnectionManagerServiceType == null) return null;
            var result = Call(device.ConnectionManagerUrl, device.ConnectionManagerServiceType, "GetProtocolInfo", new KeyValuePair<string, string>[0], false, ReadTimeoutMillis);
            return result.TryGetValue("Sink", out var sink) ? sink : null;
        }

        Dictionary<string, string> AvTransport(string action, IList<KeyValuePair<string, string>> arguments, bool logAnswer = true, int readTimeoutMillis = ReadTimeoutMillis) =>
            Call(device.AvTransportControlUrl, device.AvTransportServiceType, action, arguments, logAnswer, readTimeoutMillis);

        Dictionary<string, string> RenderingControl(string action, IList<KeyValuePair<string, string>> arguments)
        {
            if (device.RenderingControlUrl == null || device.RenderingControlServiceType == null)
                throw new UpnpException(action, 0, "The device has no RenderingControl service");
            return Call(device.RenderingControlUrl, device.RenderingControlServiceType, action, arguments, true, ReadTimeoutMillis);
        }

        Dictionary<string, string> Call(string controlUrl, string serviceType, string action, IList<KeyValuePair<string, string>> arguments, bool logAnswer, int readTimeoutMillis)
        {
            var envelope = Soap.BuildEnvelope(serviceType, action, arguments);
            if (logAnswer)
            {
                // one line, with the arguments: the full envelopes made the log too long to share
                var details = string.Join(", ", arguments.Where(a => !a.Equals(InstanceId)).Select(a => a.Key + "=" + a.Value));
                CastLog.D("SOAP -> " + action + (details.Length > 0 ? " (" + details + ")" : ""));
            }
            try
            {
                var response = LanHttp.Request(controlUrl, "POST", new[]
                {
                    P("Content-Type", Soap.ContentType),
                    P("SOAPACTION", Soap.SoapActionHeader(serviceType, action)),
                    P("User-Agent", UserAgent)
                }, Encoding.UTF8.GetBytes(envelope), readTimeoutMillis: readTimeoutMillis);
                var body = response.BodyAsString();
                if (!response.IsSuccessful) CastLog.D("SOAP <- " + action + " HTTP " + response.Code + "\n" + body);
                else if (logAnswer) CastLog.D("SOAP <- " + action + " HTTP " + response.Code);
                return Soap.ParseResponse(action, response.Code, body);
            }
            catch (UpnpException e)
            {
                CastLog.E("SOAP " + action + ": UPnP error " + e.ErrorCode + " " + e.ErrorDescription);
                throw;
            }
            catch (IOException e)
            {
                CastLog.E("SOAP " + action + ": network error", e);
                throw;
            }
        }

        /// <summary>Downloads a device description and logs it.</summary>
        public static string FetchDescription(string location)
        {
            var response = LanHttp.Request(location, headers: new[] { P("User-Agent", UserAgent) });
            var body = response.BodyAsString();
            if (!response.IsSuccessful)
            {
                CastLog.D("Device description from " + location + " (HTTP " + response.Code + "):\n" + body);
                throw new IOException("HTTP " + response.Code + " while downloading " + location);
            }
            return body;
        }
    }
}
