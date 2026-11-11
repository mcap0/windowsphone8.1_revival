using System;
using Windows.Data.Xml.Dom;
using Windows.UI.Notifications;

namespace TelegramWeb
{
    // Toasts for page notifications and the tile unread badge. Own code.
    internal static class Notifications
    {
        public static void ShowToast(string title, string body)
        {
            XmlDocument xml = ToastNotificationManager.GetTemplateContent(ToastTemplateType.ToastText02);
            XmlNodeList texts = xml.GetElementsByTagName("text");
            texts.Item(0).AppendChild(xml.CreateTextNode(title ?? ""));
            texts.Item(1).AppendChild(xml.CreateTextNode(body ?? ""));
            ToastNotificationManager.CreateToastNotifier().Show(new ToastNotification(xml));
        }

        // count <= 0 clears the badge. WP8.1 numeric badge takes 1-99.
        public static void SetBadge(int count)
        {
            BadgeUpdater updater = BadgeUpdateManager.CreateBadgeUpdaterForApplication();
            if (count <= 0)
            {
                updater.Clear();
                return;
            }
            XmlDocument xml = BadgeUpdateManager.GetTemplateContent(BadgeTemplateType.BadgeNumber);
            XmlElement badge = (XmlElement)xml.GetElementsByTagName("badge").Item(0);
            badge.SetAttribute("value", Math.Min(count, 99).ToString());
            updater.Update(new BadgeNotification(xml));
        }
    }
}
