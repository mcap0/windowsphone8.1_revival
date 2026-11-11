/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.DlnaDevice, DeviceDescriptionParser and DlnaDiscovery.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;

namespace Perle.Cast.Dlna
{
    /// <summary>A DLNA/UPnP MediaRenderer (e.g. a smart TV) which can be controlled via SOAP.</summary>
    public sealed class DlnaDevice
    {
        /// <summary>the unique device name (uuid:...), used to tell devices apart</summary>
        public string Udn { get; set; }
        /// <summary>the name to show to the user, e.g. [TV] Samsung 6 Series (43)</summary>
        public string FriendlyName { get; set; }
        public string Manufacturer { get; set; }
        public string ModelName { get; set; }
        /// <summary>the URL of the device description, from SSDP</summary>
        public string Location { get; set; }
        /// <summary>absolute URL of the AVTransport control endpoint</summary>
        public string AvTransportControlUrl { get; set; }
        public string AvTransportServiceType { get; set; }
        /// <summary>absolute URL of the RenderingControl control endpoint, if any</summary>
        public string RenderingControlUrl { get; set; }
        public string RenderingControlServiceType { get; set; }
        /// <summary>absolute URL of the ConnectionManager control endpoint, if any</summary>
        public string ConnectionManagerUrl { get; set; }
        public string ConnectionManagerServiceType { get; set; }

        /// <summary>
        /// The brand, to tell apart TVs with similar names: the first word of the manufacturer
        /// (Samsung Electronics -> Samsung), null if unknown or already in FriendlyName.
        /// </summary>
        public string Brand
        {
            get
            {
                var first = Manufacturer?.Trim().Split(' ').FirstOrDefault()?.Trim('.', ',');
                return !string.IsNullOrEmpty(first) && FriendlyName.IndexOf(first, StringComparison.OrdinalIgnoreCase) < 0 ? first : null;
            }
        }

        /// <summary>The name shown in the list of TVs, with the brand if it is not in the name.</summary>
        public string Label => Brand != null ? FriendlyName + " (" + Brand + ")" : FriendlyName;
    }

    /// <summary>Parses UPnP device description documents (the XML found at the SSDP LOCATION).</summary>
    public static class DeviceDescriptionParser
    {
        const string AvTransportPrefix = "urn:schemas-upnp-org:service:AVTransport:";
        const string RenderingControlPrefix = "urn:schemas-upnp-org:service:RenderingControl:";
        const string ConnectionManagerPrefix = "urn:schemas-upnp-org:service:ConnectionManager:";

        /// <param name="xml">the device description</param>
        /// <param name="location">the URL the description was downloaded from, used to resolve relative
        /// URLs when the document has no URLBase</param>
        /// <returns>the device, or null if it has no AVTransport service (so it cannot play videos)</returns>
        public static DlnaDevice Parse(string xml, string location)
        {
            var root = XmlUtils.Parse(xml).DocumentElement;
            var urlBase = XmlUtils.ChildText(root, "URLBase");
            var @base = !string.IsNullOrEmpty(urlBase) ? urlBase : location;
            var rootDevice = XmlUtils.Child(root, "device");
            if (rootDevice == null) return null;

            // the renderer may be an embedded device (e.g. inside a media server), look at all of them
            foreach (var device in AllDevices(rootDevice))
            {
                var serviceList = XmlUtils.Child(device, "serviceList");
                var services = serviceList != null ? XmlUtils.Children(serviceList, "service") : new List<XmlElement>();
                var avTransport = services.FirstOrDefault(s => ServiceType(s).StartsWith(AvTransportPrefix));
                if (avTransport == null) continue;
                var renderingControl = services.FirstOrDefault(s => ServiceType(s).StartsWith(RenderingControlPrefix));
                var connectionManager = services.FirstOrDefault(s => ServiceType(s).StartsWith(ConnectionManagerPrefix));
                var avTransportUrl = XmlUtils.ChildText(avTransport, "controlURL");
                if (avTransportUrl == null) continue;

                var friendly = XmlUtils.ChildText(device, "friendlyName");
                var rcUrl = renderingControl != null ? XmlUtils.ChildText(renderingControl, "controlURL") : null;
                var cmUrl = connectionManager != null ? XmlUtils.ChildText(connectionManager, "controlURL") : null;
                return new DlnaDevice
                {
                    Udn = XmlUtils.ChildText(device, "UDN") ?? location,
                    FriendlyName = !string.IsNullOrEmpty(friendly) ? friendly : HostOf(location),
                    Manufacturer = XmlUtils.ChildText(device, "manufacturer"),
                    ModelName = XmlUtils.ChildText(device, "modelName"),
                    Location = location,
                    AvTransportControlUrl = ResolveUrl(@base, avTransportUrl),
                    AvTransportServiceType = ServiceType(avTransport),
                    RenderingControlUrl = rcUrl != null ? ResolveUrl(@base, rcUrl) : null,
                    RenderingControlServiceType = renderingControl != null ? ServiceType(renderingControl) : null,
                    ConnectionManagerUrl = cmUrl != null ? ResolveUrl(@base, cmUrl) : null,
                    ConnectionManagerServiceType = connectionManager != null ? ServiceType(connectionManager) : null
                };
            }
            return null;
        }

        static List<XmlElement> AllDevices(XmlElement device)
        {
            var result = new List<XmlElement> { device };
            var deviceList = XmlUtils.Child(device, "deviceList");
            if (deviceList != null) foreach (var d in XmlUtils.Children(deviceList, "device")) result.AddRange(AllDevices(d));
            return result;
        }

        static string ServiceType(XmlElement service) => XmlUtils.ChildText(service, "serviceType") ?? "";

        static string HostOf(string url)
        {
            try { return new Uri(url).Host; } catch (Exception) { return url; }
        }

        /// <summary>
        /// Resolves a URL from a device description against the base URL, following RFC 3986.
        ///
        /// Devices use all kinds of forms: absolute URLs, absolute paths (/upnp/control/AVT) and
        /// relative paths (AVTransport/control). A base without any path (http://1.2.3.4:9197)
        /// is treated as the root directory.
        /// </summary>
        public static string ResolveUrl(string @base, string url)
        {
            var trimmed = url.Trim();
            if (trimmed.StartsWith("http://", StringComparison.OrdinalIgnoreCase) || trimmed.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                return trimmed;
            var baseUri = new Uri(@base.Trim());
            return new Uri(baseUri, trimmed).ToString();
        }
    }

    /// <summary>
    /// Finds DLNA MediaRenderers: SSDP search, then download and parse of the description of every
    /// device which answered.
    /// </summary>
    public static class DlnaDiscovery
    {
        const int DescriptionTimeoutSeconds = 15;

        /// <summary>Blocks for about timeoutMillis (plus the time needed to download the last descriptions).</summary>
        /// <param name="onDevice">called (from a background thread) for every renderer found</param>
        public static void Discover(IPAddress localAddress, Action<DlnaDevice> onDevice, long timeoutMillis = 4000)
        {
            var seenLocations = new HashSet<string>();
            var seenDevices = new HashSet<string>();
            var tasks = new List<Task>();
            using (var gate = new SemaphoreSlim(4))
            {
                try
                {
                    Ssdp.Search(localAddress, response =>
                    {
                        lock (seenLocations) if (!seenLocations.Add(response.Location)) return;
                        tasks.Add(Task.Run(() =>
                        {
                            gate.Wait();
                            try
                            {
                                var xml = DlnaClient.FetchDescription(response.Location);
                                var device = DeviceDescriptionParser.Parse(xml, response.Location);
                                if (device == null) CastLog.D(response.Location + " has no AVTransport service, ignored");
                                else
                                {
                                    bool isNew;
                                    lock (seenDevices) isNew = seenDevices.Add(device.Udn);
                                    if (isNew)
                                    {
                                        CastLog.D("Found renderer \"" + device.FriendlyName + "\" (" + device.Manufacturer + " " + device.ModelName
                                            + "): AVTransport " + device.AvTransportControlUrl + ", RenderingControl " + device.RenderingControlUrl);
                                        onDevice(device);
                                    }
                                }
                            }
                            catch (Exception e) { CastLog.E("Could not read device description " + response.Location, e); }
                            finally { gate.Release(); }
                        }));
                    }, timeoutMillis: timeoutMillis);
                }
                finally
                {
                    Task.WaitAll(tasks.ToArray(), TimeSpan.FromSeconds(DescriptionTimeoutSeconds));
                }
            }
        }
    }
}
