namespace TelegramWeb
{
    // Page-side scripts injected at document start. Own code (not derived from MetroSurf).
    internal static class UserScripts
    {
        // Prefix on console.log messages that carry a notification payload.
        // Native side (MainPage.OnConsoleMessage) recognises it and shows a toast.
        public const string NotifyPrefix = "\u0001TGWEB_NOTIFY\u0001";

        // Replaces window.Notification with a shim that reports each notification
        // to native through console.log(prefix + JSON). Permission is always "granted".
        // Single quotes only, so the C# verbatim string needs no escaping.
        public const string NotificationShim = @"
(function () {
  if (window.__tgwebNotifyInstalled) return;
  window.__tgwebNotifyInstalled = true;
  var PREFIX = '\u0001TGWEB_NOTIFY\u0001';

  function post(title, options) {
    try {
      var payload = JSON.stringify({
        t: String(title || ''),
        b: String((options && options.body) || ''),
        g: String((options && options.tag) || '')
      });
      console.log(PREFIX + payload);
    } catch (e) {}
  }

  function TgNotification(title, options) {
    post(title, options);
    this.title = String(title || '');
    this.body = String((options && options.body) || '');
    this.tag = String((options && options.tag) || '');
    this.icon = String((options && options.icon) || '');
    this.data = options ? options.data : undefined;
    this.onclick = null;
    this.onclose = null;
    this.onerror = null;
    this.onshow = null;
  }

  TgNotification.permission = 'granted';
  TgNotification.maxActions = 0;
  TgNotification.requestPermission = function (callback) {
    if (typeof callback === 'function') {
      try { callback('granted'); } catch (e) {}
    }
    return Promise.resolve('granted');
  };
  TgNotification.prototype.close = function () {};
  TgNotification.prototype.addEventListener = function () {};
  TgNotification.prototype.removeEventListener = function () {};
  TgNotification.prototype.dispatchEvent = function () { return true; };

  try {
    Object.defineProperty(window, 'Notification', { value: TgNotification, writable: true, configurable: true });
  } catch (e) {
    window.Notification = TgNotification;
  }
})();
";
    }
}
