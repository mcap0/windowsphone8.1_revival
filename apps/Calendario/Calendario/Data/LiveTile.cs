using System;
using System.Collections.Generic;
using Calendario.Logic;
using Windows.Data.Xml.Dom;
using Windows.UI.Notifications;

namespace Calendario.Data
{
    /// <summary>Medium live tile: next event today. Refreshed only while the app runs.</summary>
    public static class LiveTile
    {
        public static void Update(IEnumerable<EventItem> events, DateTime now)
        {
            EventItem next = AgendaBuilder.NextToday(events, now);
            XmlDocument doc = TileUpdateManager.GetTemplateContent(TileTemplateType.TileSquare150x150Text02);
            XmlNodeList texts = doc.GetElementsByTagName("text");

            string line1 = next == null ? "Nessun evento oggi" : next.Title;
            string line2 = next == null ? "" : (next.AllDay ? "Tutto il giorno" : TimeFormat.Hm(next.Start));
            if (texts.Length > 0) texts[0].InnerText = line1;
            if (texts.Length > 1) texts[1].InnerText = line2;

            TileUpdateManager.CreateTileUpdaterForApplication().Update(new TileNotification(doc));
        }
    }
}
