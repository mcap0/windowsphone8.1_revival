// Helpers shared by the browser test pages. ES5 only: the same pages must run in IE11, MetroSurf and our engine.
var T = (function () {
  function now() { return window.performance && performance.now ? performance.now() : Date.now(); }

  // Age of an event in ms when the page sees it (time spent queued). IE11 stamps events in epoch ms,
  // WebKit in ms since the time origin.
  function age(e) {
    var ts = e && e.timeStamp;
    if (!ts) return -1;
    var d = ts > 1e12 ? Date.now() - ts : now() - ts;
    return d >= 0 && d < 600000 ? Math.round(d) : -1;
  }

  // Calls f after the next frame has been produced (two rAFs): approximates "visible on screen".
  function afterPaint(f) { requestAnimationFrame(function () { requestAnimationFrame(f); }); }

  function engine() {
    var ua = navigator.userAgent;
    if (/Lumiaweb/i.test(ua)) return "nostro";
    if (/Metrosurf/i.test(ua)) return "MetroSurf";
    // Ours: WebKit without Chrome or Edge, as Safari on an iPhone (default) or a Mac (desktop version), or WebCore's own
    // Windows agent in builds before 0.1.0.11. No real iPhone or Mac visits these pages.
    if (/AppleWebKit/.test(ua) && /Windows NT|iPhone|Macintosh/.test(ua) && !/Chrome|Edg/.test(ua)) return "nostro";
    if (/Trident|IEMobile/i.test(ua)) return "IE11";
    return "altro";
  }

  function busy(ms) {
    var end = now() + ms, n = 0;
    while (now() < end) n++;
    return n;
  }

  function stats(values) {
    var a = [];
    for (var i = 0; i < values.length; i++) if (values[i] >= 0) a.push(values[i]);
    if (!a.length) return "n=0";
    a.sort(function (x, y) { return x - y; });
    function q(p) { return Math.round(a[Math.min(a.length - 1, Math.floor(a.length * p))]); }
    return "n=" + a.length + " mediana=" + q(0.5) + " 95%=" + q(0.95) + " max=" + Math.round(a[a.length - 1]);
  }

  function send(page, lines, done) {
    var text = "[" + page + "] " + engine() + "\n" + navigator.userAgent + "\n" + lines.join("\n");
    var x = new XMLHttpRequest();
    x.open("POST", "/browser");
    x.onreadystatechange = function () { if (x.readyState === 4 && done) done(x.status === 200); };
    x.send(text);
  }

  function $(id) { return document.getElementById(id); }

  return { now: now, age: age, afterPaint: afterPaint, engine: engine, busy: busy, stats: stats, send: send, $: $ };
})();
