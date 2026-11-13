// The service worker of sw.html (scope sw-scope/): answers a navigation with a page of its own, a load with text, a
// load from its Cache storage, and messages from the page.
var VERSION = "1";

self.addEventListener("install", function (event) {
  event.waitUntil(caches.open("sw-prova").then(function (cache) {
    return cache.put(new Request("sw-scope/cached"), new Response("dalla cache del worker", { headers: { "Content-Type": "text/plain" } }));
  }).then(function () { return self.skipWaiting(); }));
});

self.addEventListener("activate", function (event) {
  event.waitUntil(self.clients.claim());
});

self.addEventListener("fetch", function (event) {
  var url = new URL(event.request.url);
  var path = url.pathname;
  if (/\/sw-scope\/frame\.html$/.test(path)) {
    // A document made by the worker: it asks the worker two things and tells its parent what came back.
    var html = "<!doctype html><meta charset=utf-8><body>pagina del worker<script>" +
      "function go(){Promise.all([fetch('dato').then(function(r){return r.text()}),fetch('cached').then(function(r){return r.text()}),fetch('rete?x='+Date.now()).then(function(r){return r.status})])" +
      ".then(function(v){parent.postMessage({dato:v[0],cached:v[1],rete:v[2],controllato:!!navigator.serviceWorker.controller},'*')}," +
      "function(e){parent.postMessage({errore:String(e)},'*')})}go();</" + "script>";
    event.respondWith(new Response(html, { headers: { "Content-Type": "text/html; charset=utf-8" } }));
    return;
  }
  if (/\/sw-scope\/dato$/.test(path)) {
    event.respondWith(new Response("dal worker " + VERSION, { headers: { "Content-Type": "text/plain" } }));
    return;
  }
  if (/\/sw-scope\/cached$/.test(path)) {
    event.respondWith(caches.match(event.request).then(function (r) { return r || new Response("non in cache", { status: 404 }); }));
    return;
  }
  // Anything else goes to the network as if there were no worker (the server answers 404 for sw-scope/rete).
});

self.addEventListener("message", function (event) {
  event.source.postMessage({ eco: event.data, da: "worker" });
});
