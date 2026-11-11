package dumbphone.pixelfoto;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.os.IBinder;
import android.os.PowerManager;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/**
 * Foreground service: HTTP on 0.0.0.0:8792 (PLAN.md §58.2), UDP discovery on 8793, hourly cleanup.
 *
 * POST /begin {name,size,sha256,taken}  → {id,status:"upload",offset} | {status:"done",file}
 * PUT  /chunk?id=&offset=  (body ≤ 8 MB) → {offset} | 409 {offset}
 * GET  /state?id=                       → {status,offset,size} | {status:"done",file}
 * POST /end?id=                         → {status:"saved",file,taken} | {status:"upload",offset}
 * GET  /list                            → [{sha256,name,file,received,state}]
 * GET  /ts                             → {report: Tailscale state, login link, `tailscale status`}
 * GET  /ping (no token)                 → {app:"pixelfoto",v:1}
 */
public final class Receiver extends Service {
    static final int PORT = 8792, UDP_PORT = 8793;
    static final long MAX_CHUNK = 8L << 20;
    private static final String CHANNEL = "ricevitore";

    private Store store;
    private Ts ts;
    private byte[] token;
    private ServerSocket server;
    private DatagramSocket udp;
    private final ExecutorService pool = Executors.newCachedThreadPool();
    private final ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor();
    private PowerManager.WakeLock wake;
    private WifiManager.WifiLock wifi;
    private volatile boolean running;

    @Override
    public IBinder onBind(Intent i) { return null; }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Ricevitore", NotificationManager.IMPORTANCE_LOW));
        startForeground(1, note("avvio…"));
        try {
            try (InputStream in = getAssets().open("token")) { token = Exif.readAll(in); }
            store = new Store(this);
        } catch (IOException e) {
            Log.i("avvio fallito: " + e);
            stopSelf();
            return;
        }
        wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "pixelfoto:server");
        wake.acquire();
        wifi = ((WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE))
                .createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "pixelfoto");
        wifi.acquire();
        running = true;
        pool.execute(this::acceptLoop);
        pool.execute(this::udpLoop);
        ts = new Ts(this);
        ts.start();
        timer.scheduleAtFixedRate(() -> {
            try { store.cleanup(); } catch (Throwable t) { Log.i("pulizia: " + t); }
            updateNote();
        }, 1, 60, TimeUnit.MINUTES);
        updateNote();
        Log.i("in ascolto su :" + PORT);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) { return START_STICKY; }

    @Override
    public void onDestroy() {
        running = false;
        if (ts != null) ts.stop();
        try { if (server != null) server.close(); } catch (IOException ignored) { }
        if (udp != null) udp.close();
        pool.shutdownNow();
        timer.shutdownNow();
        if (wake != null && wake.isHeld()) wake.release();
        if (wifi != null && wifi.isHeld()) wifi.release();
        super.onDestroy();
    }

    private Notification note(String text) {
        return new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_menu_camera)
                .setContentTitle("Foto dal Lumia").setContentText(text).setOngoing(true).build();
    }

    private void updateNote() {
        if (store == null) return;
        getSystemService(NotificationManager.class).notify(1, note("porta " + PORT + " · " + store.count() + " ricevute"));
    }

    // ---- discovery: "FOTOPIXEL?" → "FOTOPIXEL <port>" ----

    private void udpLoop() {
        try {
            udp = new DatagramSocket(null);
            udp.setReuseAddress(true);
            udp.bind(new InetSocketAddress(UDP_PORT));
            byte[] buf = new byte[64];
            while (running) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                udp.receive(p);
                String s = new String(p.getData(), 0, p.getLength(), StandardCharsets.US_ASCII).trim();
                if (!s.equals("FOTOPIXEL?")) continue;
                byte[] r = ("FOTOPIXEL " + PORT).getBytes(StandardCharsets.US_ASCII);
                udp.send(new DatagramPacket(r, r.length, p.getSocketAddress()));
            }
        } catch (IOException e) {
            if (running) Log.i("udp: " + e);
        }
    }

    // ---- HTTP ----

    private void acceptLoop() {
        while (running) {
            try {
                if (server == null) {
                    server = new ServerSocket();
                    server.setReuseAddress(true);
                    server.bind(new InetSocketAddress(PORT));
                }
                Socket s = server.accept();
                pool.execute(() -> serve(s));
            } catch (IOException e) {
                if (!running) return;
                Log.i("accept: " + e);
                try { if (server != null) server.close(); } catch (IOException ignored) { }
                server = null;
                try { Thread.sleep(2000); } catch (InterruptedException x) { return; }
            }
        }
    }

    private static String readLine(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') break;
            if (c != '\r') b.write(c);
            if (b.size() > 8192) throw new IOException("riga troppo lunga");
        }
        if (c < 0 && b.size() == 0) return null;
        return b.toString("ISO-8859-1");
    }

    private void serve(Socket s) {
        try (Socket sock = s) {
            sock.setSoTimeout(120_000);
            sock.setTcpNoDelay(true);
            InputStream in = new BufferedInputStream(sock.getInputStream(), 1 << 16);
            OutputStream out = sock.getOutputStream();
            while (true) {
                String line = readLine(in);
                if (line == null || line.isEmpty()) return;
                String[] rl = line.split(" ");
                if (rl.length < 3) return;
                Map<String, String> h = new HashMap<>();
                String hl;
                while ((hl = readLine(in)) != null && !hl.isEmpty()) {
                    int k = hl.indexOf(':');
                    if (k > 0) h.put(hl.substring(0, k).trim().toLowerCase(Locale.ROOT), hl.substring(k + 1).trim());
                }
                long len = h.containsKey("content-length") ? Long.parseLong(h.get("content-length")) : 0;
                boolean close = "close".equalsIgnoreCase(h.get("connection")) || rl[2].equals("HTTP/1.0");
                Bounded body = new Bounded(in, len);
                int[] code = {200};
                String resp;
                try {
                    resp = route(rl[0], rl[1], h, body, code);
                } catch (Store.Conflict c) {
                    code[0] = 409;
                    resp = new JSONObject().put("error", c.getMessage()).put("offset", c.have).toString();
                } catch (Store.BadRequest b) {
                    code[0] = 400;
                    resp = new JSONObject().put("error", b.getMessage()).toString();
                } catch (SecurityException e) {
                    code[0] = 403;
                    resp = "{\"error\":\"token\"}";
                } catch (Exception e) {
                    Log.i(rl[0] + " " + rl[1] + ": " + e);
                    code[0] = 500;
                    resp = new JSONObject().put("error", String.valueOf(e)).toString();
                }
                body.skipRest();
                byte[] b = resp.getBytes(StandardCharsets.UTF_8);
                String head = "HTTP/1.1 " + code[0] + (code[0] == 200 ? " OK" : " Error")
                        + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + b.length
                        + (close ? "\r\nConnection: close" : "") + "\r\n\r\n";
                out.write(head.getBytes(StandardCharsets.ISO_8859_1));
                out.write(b);
                out.flush();
                if (close) return;
            }
        } catch (Exception e) {
            // Connection dropped: the client resumes with /state.
        }
    }

    private String route(String method, String target, Map<String, String> h, Bounded body, int[] code)
            throws Exception {
        int q = target.indexOf('?');
        String path = q < 0 ? target : target.substring(0, q);
        Map<String, String> qs = new HashMap<>();
        if (q >= 0) for (String kv : target.substring(q + 1).split("&")) {
            int e = kv.indexOf('=');
            if (e > 0) qs.put(kv.substring(0, e), URLDecoder.decode(kv.substring(e + 1), "UTF-8"));
        }
        if (path.equals("/ping")) return new JSONObject().put("app", "pixelfoto").put("v", 1).toString();
        String t = h.get("x-token");
        if (t == null || !MessageDigest.isEqual(t.getBytes(StandardCharsets.UTF_8), token)) throw new SecurityException();
        String id = qs.get("id");
        switch (method + " " + path) {
            case "POST /begin":
                if (body.left > 65536) throw new Store.BadRequest("richiesta troppo grande");
                return store.begin(new JSONObject(new String(body.readAll(), StandardCharsets.UTF_8))).toString();
            case "PUT /chunk": {
                if (!Store.validId(id)) throw new Store.BadRequest("id");
                if (body.left > MAX_CHUNK) throw new Store.BadRequest("blocco troppo grande");
                long off = Long.parseLong(qs.getOrDefault("offset", "-1"));
                long now = store.chunk(id, off, body, body.left);
                return new JSONObject().put("id", id).put("offset", now).toString();
            }
            case "GET /state":
                if (!Store.validId(id)) throw new Store.BadRequest("id");
                return store.state(id).toString();
            case "POST /end": {
                if (!Store.validId(id)) throw new Store.BadRequest("id");
                String r = store.end(id).toString();
                updateNote();
                return r;
            }
            case "GET /ts":
                return new JSONObject().put("report", ts.report()).toString();
            case "GET /list":
                return store.list(Integer.parseInt(qs.getOrDefault("n", "200"))).toString();
            default:
                code[0] = 404;
                return "{\"error\":\"not found\"}";
        }
    }

    /** The request body: at most Content-Length bytes of the connection. */
    static final class Bounded extends InputStream {
        final InputStream in;
        long left;
        Bounded(InputStream in, long len) { this.in = in; left = len; }
        @Override public int read() throws IOException {
            if (left <= 0) return -1;
            int c = in.read();
            if (c >= 0) left--;
            return c;
        }
        @Override public int read(byte[] b, int o, int n) throws IOException {
            if (left <= 0) return -1;
            int r = in.read(b, o, (int) Math.min(n, left));
            if (r > 0) left -= r;
            return r;
        }
        byte[] readAll() throws IOException { return Exif.readAll(this); }
        void skipRest() throws IOException {
            byte[] buf = new byte[8192];
            while (left > 0 && read(buf, 0, buf.length) > 0) { }
        }
    }
}
