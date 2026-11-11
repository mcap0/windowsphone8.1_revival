package dumbphone.ponte;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// Our own push server for the Lumia, in place of Microsoft's WNS (PLAN §69.8, §71.2): the Java port of tools/wnsfake
// (protocol notes there), running on the Ponte for good. The phone's WNS client (WPNPRV, registry
// PushNotifications\Server) reaches it through Tailscale Funnel: ponte-pixel.<tailnet>.ts.net:443, TLS ended by
// tailscaled, plain TCP to 127.0.0.1:8799. Pushes come from the bridge on 127.0.0.1:8800: POST /push/<channel>
// (X-WNS-Type, X-WNS-Tag, X-WNS-Group, X-TTL), POST /trust/<channel>, GET /status.
//
// The phone port is public (Funnel), so pushes go only to TRUSTED connections, and a stranger never learns a channel:
//  - a session id we issued to a trusted connection, back in a fast reconnect;
//  - a full CNT whose Last-Msg-Id is a push a trusted connection acked (ids grow by random steps: not guessable);
//  - a channel renewal (PUT) carrying the id of a channel we issued to a trusted session (ids: 128 random bits);
//  - the bridge's /trust/<channel>: the Lumia app, over its authenticated link, handed that channel to the bridge, so
//    the session it was issued on is the Lumia's (or, for a channel from before, the only untrusted WNS connection).
// An untrusted session asking for a channel gets a new one, held until that session is trusted. No message text in
// the log; the account ticket (ATH) is never read, logged or stored.
public final class WnsServer {
    static final int PHONE_PORT = 8799, API_PORT = 8800, MAX_CONNS = 32;
    static final File STATE = new File(Main.DIR, "wns.json");
    static final String URL_BASE = "http://127.0.0.1:" + API_PORT + "/push/";
    static final SecureRandom rnd = new SecureRandom();

    // ---- state (one lock: small and rare) ----
    static final Object lock = new Object();
    static final Map<String, JSONObject> channels = new HashMap<>();   // id -> {appkey, created, exp, session}
    static final LinkedHashSet<String> trustedSessions = new LinkedHashSet<>();
    static final Map<String, String> roles = new java.util.LinkedHashMap<>();   // session id -> WNS / QOS
    static final LinkedHashSet<Long> acked = new LinkedHashSet<>();     // msg ids acked by trusted connections
    static final List<Object[]> queue = new ArrayList<>();               // {expiry ms, cid, body, kind, tag, group}
    static final List<Phone> live = new ArrayList<>();
    static long lastMid;

    public static void main(String[] args) throws Exception {
        load();
        log("avvio: canali " + channels.size() + ", sessioni fidate " + trustedSessions.size());
        Thread api = new Thread(WnsServer::api, "wns-api");
        api.setDaemon(true);
        api.start();
        try (ServerSocket ss = new ServerSocket()) {
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), PHONE_PORT));
            log("telefono su 127.0.0.1:" + PHONE_PORT + ", API su 127.0.0.1:" + API_PORT);
            while (true) {
                Socket s = ss.accept();
                synchronized (lock) {
                    if (live.size() >= MAX_CONNS) { s.close(); continue; }
                }
                new Thread(() -> new Phone(s).run(), "wns-phone").start();
            }
        }
    }

    // ---- persistence ----

    static void load() {
        try {
            JSONObject o = new JSONObject(new String(Files.readAllBytes(STATE.toPath()), StandardCharsets.UTF_8));
            JSONObject ch = o.optJSONObject("channels");
            if (ch != null) for (java.util.Iterator<String> it = ch.keys(); it.hasNext(); ) {
                String k = it.next();
                channels.put(k, ch.getJSONObject(k));
            }
            JSONArray t = o.optJSONArray("trusted");
            for (int i = 0; t != null && i < t.length(); i++) trustedSessions.add(t.getString(i));
            JSONObject r = o.optJSONObject("roles");
            if (r != null) for (java.util.Iterator<String> it = r.keys(); it.hasNext(); ) {
                String k = it.next();
                roles.put(k, r.getString(k));
            }
            JSONArray a = o.optJSONArray("acked");
            for (int i = 0; a != null && i < a.length(); i++) acked.add(a.getLong(i));
            lastMid = o.optLong("lastMid", 0);
        } catch (java.nio.file.NoSuchFileException e) {
            // first start
        } catch (Exception e) {
            log("stato non letto: " + e);
        }
    }

    static void save() {   // under lock
        try {
            // expired channels go; past 200 (strangers can ask for channels), the ones of untrusted sessions too
            String now = utc(System.currentTimeMillis());
            channels.values().removeIf(c -> c.optString("exp", "9").compareTo(now) < 0);
            if (channels.size() > 200)
                channels.values().removeIf(c -> !trustedSessions.contains(c.optString("session")));
            JSONObject o = new JSONObject();
            JSONObject ch = new JSONObject();
            for (Map.Entry<String, JSONObject> e : channels.entrySet()) ch.put(e.getKey(), e.getValue());
            o.put("channels", ch);
            while (trustedSessions.size() > 40) trustedSessions.remove(trustedSessions.iterator().next());
            o.put("trusted", new JSONArray(trustedSessions));
            // roles of every session (fast reconnects skip BND): the last 40
            while (roles.size() > 40) roles.remove(roles.keySet().iterator().next());
            JSONObject r = new JSONObject();
            for (Map.Entry<String, String> e : roles.entrySet()) r.put(e.getKey(), e.getValue());
            o.put("roles", r);
            while (acked.size() > 50) acked.remove(acked.iterator().next());
            o.put("acked", new JSONArray(acked));
            o.put("lastMid", lastMid);
            File tmp = new File(STATE.getPath() + ".tmp");
            try (FileOutputStream f = new FileOutputStream(tmp)) {
                f.write(o.toString(1).getBytes(StandardCharsets.UTF_8));
            }
            if (!tmp.renameTo(STATE)) log("stato non salvato (rename)");
        } catch (Exception e) {
            log("stato non salvato: " + e);
        }
    }

    // ---- trust ----

    static void trust(Phone p, String why) {   // under lock
        if (p.session == null || p.trusted) return;
        p.trusted = true;
        trustedSessions.add(p.session);
        if (p.role != null) roles.put(p.session, p.role);
        save();
        log("fidata " + p.addr + " (" + why + ")");
        flushQueue();
    }

    // ---- one phone connection ----

    static final class Phone {
        final Socket sock;
        final String addr;
        String session, role;
        boolean trusted;
        final Map<Long, CountDownLatch> acks = new ConcurrentHashMap<>();
        final long since = System.currentTimeMillis();
        volatile long lastRx = since;

        Phone(Socket s) {
            sock = s;
            addr = "#" + Integer.toHexString(System.identityHashCode(s));   // through Funnel every peer is 127.0.0.1
        }

        // One write per message: behind Funnel every write becomes its own TLS record (tailscaled ends TLS), and the
        // phone's client drops a connection whose message is split across records (19:27: header and body in two
        // writes -> the phone closed right after BND, again and again).
        void close() {
            try { sock.close(); } catch (Exception ignored) {}
        }

        void send(String verb, int trid, String target, byte[] payload) throws Exception {
            byte[] head = (verb + " " + trid + " " + target + " " + payload.length + "\r\n").getBytes(StandardCharsets.ISO_8859_1);
            byte[] msg = new byte[head.length + payload.length];
            System.arraycopy(head, 0, msg, 0, head.length);
            System.arraycopy(payload, 0, msg, head.length, payload.length);
            synchronized (this) {
                OutputStream o = sock.getOutputStream();
                o.write(msg);
                o.flush();
            }
        }

        void send(String verb, int trid, String target, String payload) throws Exception {
            send(verb, trid, target, payload.getBytes(StandardCharsets.UTF_8));
        }

        void run() {
            synchronized (lock) { live.add(this); }
            log("connessione " + addr);
            try {
                sock.setTcpNoDelay(true);
                InputStream in = sock.getInputStream();
                ByteArrayOutputStream line = new ByteArrayOutputStream();
                while (true) {
                    line.reset();
                    int c, prev = -1;
                    while ((c = in.read()) >= 0) {
                        if (prev == '\r' && c == '\n') break;
                        if (prev >= 0) line.write(prev);
                        prev = c;
                        if (line.size() > 4096) throw new Exception("riga troppo lunga");
                    }
                    if (c < 0) break;
                    lastRx = System.currentTimeMillis();
                    String[] parts = line.toString("ISO-8859-1").split(" ");
                    if (parts.length != 4) throw new Exception("comando non valido: " + shown(line.toString("ISO-8859-1")));
                    int len = Integer.parseInt(parts[3]);
                    if (len < 0 || len > 65536) throw new Exception("lunghezza " + len);
                    byte[] payload = new byte[len];
                    for (int got = 0; got < len; ) {
                        int n = in.read(payload, got, len - got);
                        if (n < 0) throw new Exception("chiusa a metà");
                        got += n;
                    }
                    handle(parts[0], Integer.parseInt(parts[1]), parts[2], payload);
                }
            } catch (Exception e) {
                log("errore " + addr + " " + e.getMessage());
            } finally {
                synchronized (lock) { live.remove(this); }
                log("chiusa " + addr + " dopo " + (System.currentTimeMillis() - since) / 1000 + " s");
                try { sock.close(); } catch (Exception ignored) {}
            }
        }

        void handle(String verb, int trid, String target, byte[] payload) throws Exception {
            if (verb.equals("ATH")) {   // the account ticket: never logged, never stored
                log("<- " + addr + " ATH " + trid + " " + target + " (ticket ignorato)");
                send("ATH", trid, target, new byte[0]);
                return;
            }
            String text = new String(payload, StandardCharsets.UTF_8);
            Map<String, String> hdrs = new HashMap<>();
            String body = headers(text, hdrs);
            String ctx = hdrs.containsKey("Context") ? "Context: " + hdrs.get("Context") + "\r\n" : "";
            String kind = target.substring(target.lastIndexOf('\\') + 1);
            if (!verb.equals("PNG")) log("<- " + addr + " " + verb + " " + trid + " " + target + " " + shown(text));
            switch (verb) {
                case "CNT": {
                    String old = xml(body, "id");
                    String lastMsg = hdrs.get("Last-Msg-Id");
                    synchronized (lock) {
                        session = old != null ? old : hex(8);
                        role = roles.get(session);
                    }
                    send("CNT", trid, target, "\r\n<connect-response><session><id>" + session + "</id><timeout>3600"
                            + "</timeout></session></connect-response>");
                    synchronized (lock) {
                        if (old != null)
                            for (Phone p : new ArrayList<>(live))
                                if (p != this && old.equals(p.session)) {
                                    log("sostituita " + p.addr + " da " + addr);
                                    p.close();
                                }
                        if (old != null && trustedSessions.contains(old)) trust(this, "sessione nota");
                        else if (lastMsg != null && !lastMsg.equals("0") && acked.contains(parseHex(lastMsg)))
                            trust(this, "Last-Msg-Id noto");
                    }
                    break;
                }
                case "BND":
                    synchronized (lock) {
                        role = kind;
                        if (session != null) { roles.put(session, role); save(); }
                    }
                    send("BND", trid, target, "\r\n");
                    synchronized (lock) { if (trusted) flushQueue(); }
                    break;
                case "PNG":
                    send("PNG", trid, target, new byte[0]);
                    break;
                case "GET":
                case "PUT":
                    if (kind.equals("CHANNEL")) {
                        String appkey = xml(body, "appkey");
                        String want = xml(body, "id");
                        String id;
                        JSONObject c;
                        synchronized (lock) {
                            JSONObject known = want != null ? channels.get(want) : null;
                            // a renewal carries the channel id: only the phone holding it knows it
                            if (!trusted && known != null && trustedSessions.contains(known.optString("session")))
                                trust(this, "rinnovo di un canale fidato");
                            id = known != null ? want : null;   // the asker knows this id already: nothing leaks
                            // one channel per app key, but only a trusted session gets an existing one back
                            if (id == null && trusted && appkey != null)
                                for (Map.Entry<String, JSONObject> e : channels.entrySet())
                                    if (appkey.equals(e.getValue().optString("appkey"))) { id = e.getKey(); break; }
                            if (id == null) {   // a new channel: deliverable once this session is trusted
                                id = hex(16);
                                channels.put(id, new JSONObject().put("appkey", appkey == null ? "" : appkey)
                                        .put("created", utc(System.currentTimeMillis())));
                            }
                            c = channels.get(id);
                            if (trusted || !c.has("session")) c.put("session", session);
                            c.put("exp", utc(System.currentTimeMillis() + 30L * 86400000));
                            save();
                        }
                        log("canale " + id + " per " + appkey + (trusted ? "" : " (sessione non ancora fidata)"));
                        send(verb, trid, target, ctx + "\r\n<channel><id>" + id + "</id><url>" + URL_BASE + id
                                + "</url><exp>" + c.getString("exp") + "</exp></channel>");
                        break;
                    }
                    send(verb, trid, target, ctx + "\r\n");
                    break;
                case "DEL":
                    if (kind.equals("CHANNEL") && trusted) {
                        Matcher m = Pattern.compile("<id>(.*?)</id>").matcher(body);
                        synchronized (lock) {
                            while (m.find()) channels.remove(m.group(1));
                            save();
                        }
                    }
                    send(verb, trid, target, ctx + "\r\n");
                    break;
                case "ACK": {
                    try {
                        long id = parseHex(hdrs.getOrDefault("Msg-Id", ""));
                        CountDownLatch l = acks.remove(id);
                        if (l != null) {
                            l.countDown();
                            synchronized (lock) { if (trusted) { acked.add(id); save(); } }
                        }
                    } catch (Exception ignored) {
                    }
                    send(verb, trid, target, new byte[0]);
                    break;
                }
                default:   // OPTIONS, FILTER, OUT...: acknowledged, nothing to do
                    send(verb, trid, target, ctx + "\r\n");
            }
        }

        CountDownLatch notify(String cid, byte[] body, String kind, String tag, String group) throws Exception {
            long n;
            synchronized (lock) {
                // strictly increasing (the phone reports the last one it saw) by random steps: not guessable
                lastMid = Math.max(lastMid + 1 + rnd.nextInt(0xffff), System.currentTimeMillis() * 1000);
                n = lastMid;
            }
            CountDownLatch l = new CountDownLatch(1);
            acks.put(n, l);
            String hdr = "Context: " + hex(8) + "\r\nMsg-Id: " + String.format("%016x", n) + "\r\nAck: true\r\nTime: "
                    + utc(System.currentTimeMillis()) + "\r\nType: " + kind + "\r\nChannel: " + cid + "\r\n"
                    + (tag != null ? "Tag: " + tag + "\r\n" : "") + (group != null ? "Group: " + group + "\r\n" : "") + "\r\n";
            byte[] h = hdr.getBytes(StandardCharsets.UTF_8);
            byte[] all = new byte[h.length + body.length];
            System.arraycopy(h, 0, all, 0, h.length);
            System.arraycopy(body, 0, all, h.length, body.length);
            send("NFY", 0, "WNS\\NOTIF", all);
            log("-> " + addr + " NFY " + kind + " " + body.length + " byte");
            return l;
        }
    }

    // ---- delivery ----

    static List<Phone> targets() {   // under lock: trusted, on the WNS connection (not the phone's QOS one)
        List<Phone> wns = new ArrayList<>(), other = new ArrayList<>();
        for (Phone p : live) {
            if (!p.trusted) continue;
            if ("WNS".equals(p.role)) wns.add(p);
            else if (!"QOS".equals(p.role)) other.add(p);
        }
        return wns.isEmpty() ? other : wns;
    }

    static List<Object[]> deliver(String cid, byte[] body, String kind, String tag, String group) {
        List<Phone> to;
        synchronized (lock) { to = targets(); }
        List<Object[]> sent = new ArrayList<>();
        for (Phone p : to) {
            try {
                sent.add(new Object[] {p, p.notify(cid, body, kind, tag, group)});
            } catch (Exception e) {
                log("invio a " + p.addr + ": " + e.getMessage());
            }
        }
        return sent;
    }

    static void flushQueue() {   // under lock
        if (targets().isEmpty() || queue.isEmpty()) return;
        long now = System.currentTimeMillis();
        List<Object[]> items = new ArrayList<>(queue);
        queue.clear();
        // one at a time, each after the previous one's ACK (or 3 s): never several messages in one TLS record
        new Thread(() -> {
            for (Object[] q : items) {
                if ((Long) q[0] <= now) continue;
                for (Object[] s : deliver((String) q[1], (byte[]) q[2], (String) q[3], (String) q[4], (String) q[5]))
                    try { ((CountDownLatch) s[1]).await(3, TimeUnit.SECONDS); } catch (InterruptedException ignored) {}
            }
        }, "wns-flush").start();
    }

    // ---- HTTP API, loopback only ----

    static void api() {
        try (ServerSocket ss = new ServerSocket()) {
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), API_PORT));
            while (true) {
                Socket s = ss.accept();
                new Thread(() -> request(s), "wns-http").start();
            }
        } catch (Exception e) {
            log("API: " + e);
            System.exit(3);
        }
    }

    static void request(Socket s) {
        try (Socket sock = s) {
            sock.setSoTimeout(15000);
            InputStream in = sock.getInputStream();
            ByteArrayOutputStream head = new ByteArrayOutputStream();
            int c, m = 0;
            while ((c = in.read()) >= 0) {
                head.write(c);
                m = (c == '\r' || c == '\n') ? m + 1 : 0;
                if (m == 4 || head.size() > 8192) break;
            }
            String[] lines = head.toString("ISO-8859-1").split("\r\n");
            String[] rl = lines[0].split(" ");
            Map<String, String> h = new HashMap<>();
            for (int i = 1; i < lines.length; i++) {
                int k = lines[i].indexOf(':');
                if (k > 0) h.put(lines[i].substring(0, k).trim().toLowerCase(Locale.ROOT), lines[i].substring(k + 1).trim());
            }
            int len = Integer.parseInt(h.getOrDefault("content-length", "0"));
            byte[] body = new byte[Math.max(0, Math.min(len, 65536))];
            for (int got = 0; got < body.length; ) {
                int n = in.read(body, got, body.length - got);
                if (n < 0) break;
                got += n;
            }
            JSONObject res;
            int code;
            String method = rl[0], path = rl.length > 1 ? rl[1] : "";
            Matcher push = Pattern.compile("/push/([0-9a-f]{32})").matcher(path);
            Matcher tr = Pattern.compile("/trust/([0-9a-f]{32})").matcher(path);
            if (method.equals("GET") && path.equals("/status")) {
                code = 200;
                res = status();
            } else if (method.equals("GET") && path.equals("/channels")) {
                code = 200;
                res = channelList();
            } else if (method.equals("POST") && push.matches()) {
                Object[] r = push(push.group(1), body, h);
                code = (Integer) r[0];
                res = (JSONObject) r[1];
            } else if (method.equals("POST") && tr.matches()) {
                code = 200;
                res = new JSONObject().put("fidata", trustChannel(tr.group(1)));
            } else {
                code = 404;
                res = new JSONObject();
            }
            byte[] out = res.toString().getBytes(StandardCharsets.UTF_8);
            OutputStream o = sock.getOutputStream();
            o.write(("HTTP/1.1 " + code + " X\r\nContent-Type: application/json\r\nContent-Length: " + out.length
                    + "\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.ISO_8859_1));
            o.write(out);
            o.flush();
        } catch (Exception e) {
            log("HTTP: " + e);
        }
    }

    static Object[] push(String cid, byte[] body, Map<String, String> h) throws Exception {
        String kind = h.getOrDefault("x-wns-type", "wns/raw");
        String tag = h.get("x-wns-tag"), group = h.get("x-wns-group");
        if (!kind.matches("wns/(raw|toast|tile|badge)")) return new Object[] {400, new JSONObject().put("errore", "X-WNS-Type")};
        if ((tag != null && tag.length() > 16) || (group != null && group.length() > 16))
            return new Object[] {400, new JSONObject().put("errore", "tag/group oltre 16")};
        synchronized (lock) {
            if (!channels.containsKey(cid)) return new Object[] {410, new JSONObject().put("errore", "canale sconosciuto")};
        }
        long t0 = System.currentTimeMillis();
        List<Object[]> sent = deliver(cid, body, kind, tag, group);
        if (sent.isEmpty()) {
            int ttl = Integer.parseInt(h.getOrDefault("x-ttl", "60"));
            synchronized (lock) { queue.add(new Object[] {t0 + ttl * 1000L, cid, body, kind, tag, group}); }
            log("push " + kind + " in coda (" + ttl + " s): nessuna connessione fidata");
            return new Object[] {202, new JSONObject().put("stato", "in coda")};
        }
        long deadline = t0 + 10000;
        while (System.currentTimeMillis() < deadline) {
            for (Object[] s : sent)
                if (((CountDownLatch) s[1]).await(0, TimeUnit.MILLISECONDS)) {
                    long ms = System.currentTimeMillis() - t0;
                    log("ack " + kind + " in " + ms + " ms");
                    return new Object[] {200, new JSONObject().put("stato", "ack").put("ms", ms)};
                }
            Thread.sleep(5);
        }
        int ttl = Integer.parseInt(h.getOrDefault("x-ttl", "60"));
        synchronized (lock) {
            queue.add(new Object[] {t0 + ttl * 1000L, cid, body, kind, tag, group});
            for (Object[] s : sent) {
                Phone p = (Phone) s[0];
                log("nessun ack da " + p.addr + " in 10 s: chiusa, push di nuovo in coda");
                p.close();
            }
        }
        return new Object[] {202, new JSONObject().put("stato", "nessun ack in 10 s: in coda per la prossima connessione")};
    }

    // The bridge got this channel from the Lumia app over its authenticated link: its session is the Lumia's.
    static String trustChannel(String cid) {
        synchronized (lock) {
            JSONObject c = channels.get(cid);
            if (c == null) return "canale sconosciuto";
            String s = c.optString("session", "");
            for (Phone p : live)
                if (s.equals(p.session)) { trust(p, "canale consegnato dall'app"); return "sì"; }
            if (!s.isEmpty() && !trustedSessions.contains(s)) { trustedSessions.add(s); save(); return "sessione"; }
            // a channel from before (or its session is gone): the only untrusted WNS connection is the Lumia's
            // (role unknown counts: a fast reconnect of a session we never saw bind; the QOS one does not)
            Phone only = null;
            int n = 0;
            for (Phone p : live)
                if (!p.trusted && p.session != null && !"QOS".equals(p.role)) { only = p; n++; }
            if (n == 1) { trust(only, "unica connessione WNS non fidata, canale dall'app"); return "sì"; }
            return n == 0 ? "già fidata o assente" : "ambigua (" + n + ")";
        }
    }

    // For the bridge's own services on loopback (Calls: the Lumia call app's channel, found by its app key).
    static JSONObject channelList() throws Exception {
        synchronized (lock) {
            JSONArray a = new JSONArray();
            for (Map.Entry<String, JSONObject> e : channels.entrySet())
                a.put(new JSONObject().put("id", e.getKey()).put("appkey", e.getValue().optString("appkey"))
                        .put("fidato", trustedSessions.contains(e.getValue().optString("session")))
                        .put("creato", e.getValue().optString("created")));
            return new JSONObject().put("canali", a);
        }
    }

    static JSONObject status() throws Exception {
        synchronized (lock) {
            JSONArray conns = new JSONArray();
            for (Phone p : live)
                conns.put(new JSONObject().put("conn", p.addr).put("ruolo", p.role).put("fidata", p.trusted)
                        .put("da_s", (System.currentTimeMillis() - p.since) / 1000)
                        .put("ultimo_rx_s", (System.currentTimeMillis() - p.lastRx) / 1000));
            return new JSONObject().put("connessioni", conns).put("canali", channels.size()).put("in_coda", queue.size());
        }
    }

    // ---- helpers ----

    static String headers(String payload, Map<String, String> out) {
        int sep = payload.indexOf("\r\n\r\n");
        if (sep < 0) return payload.startsWith("\r\n") ? payload.substring(2) : payload;
        for (String line : payload.substring(0, sep).split("\r\n")) {
            int k = line.indexOf(':');
            if (k > 0) out.put(line.substring(0, k).trim(), line.substring(k + 1).trim());
        }
        return payload.substring(sep + 4);
    }

    static String xml(String body, String tag) {
        Matcher m = Pattern.compile("<" + tag + ">(.*?)</" + tag + ">", Pattern.DOTALL).matcher(body);
        return m.find() ? m.group(1) : null;
    }

    static long parseHex(String s) { return Long.parseUnsignedLong(s.trim(), 16); }

    static String hex(int bytes) {
        byte[] b = new byte[bytes];
        rnd.nextBytes(b);
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    static String utc(long ms) {
        SimpleDateFormat f = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return f.format(new Date(ms));
    }

    static final Pattern LONG_BLOB = Pattern.compile("[A-Za-z0-9+/=_%.!*-]{100,}");

    static String shown(String payload) {   // long opaque runs as their length; no message bodies here (NFY is ours)
        String s = LONG_BLOB.matcher(payload).replaceAll("[blob]");
        StringBuilder b = new StringBuilder();
        for (char ch : s.toCharArray()) b.append(ch >= 32 && ch < 127 ? ch : ch == '\n' ? '|' : ' ');
        return b.length() > 300 ? b.substring(0, 300) + "..." : b.toString();
    }

    static synchronized void log(String line) {
        String stamp = new SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT).format(new Date());
        System.out.println(stamp + " " + line);
        System.out.flush();
    }
}
