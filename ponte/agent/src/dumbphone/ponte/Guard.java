package dumbphone.ponte;

import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

// The door of the bridge: owns the client port 8791 (LAN and tailnet) on its own, in a process that needs nothing
// but itself, so the Lumia can always reach the phone. Agent up (127.0.0.1:18791): frames are relayed
// untouched. Agent down: it answers the handshake itself with state "error" and the reason.
// In both cases it watches for the client's "restart" and does it from here, so a hung agent restarts too.
public final class Guard {
    // CONTROL (loopback): "start", "stop", "restart", "log", one line each, for the Ponte app. The app needs no adb
    // while this process lives: its adb key is the one thing that can be revoked by the system.
    private static final int PORT = 8791, AGENT = 18791, CONTROL = 8792;
    private static final int PEEK_MAX = 4096;   // a restart request is tiny: bigger frames are only forwarded
    // agent connected but silent after the hello: hung, restarted. It can legitimately take a while right after
    // it starts, so the wait is long and two such restarts are never closer than GAP_MS (no restart loop).
    private static final long HUNG_MS = 45000, GAP_MS = 5 * 60000;
    private static volatile long lastHung;
    private static final int CLIENT_SILENCE_MS = 45000;
    private static final String DIR = Main.DIR;
    private static byte[] encKey, macKey;
    private static final SecureRandom rnd = new SecureRandom();

    public static void main(String[] args) throws Exception {
        byte[] secret = Main.hex(new String(Files.readAllBytes(new File(DIR, "key").toPath()), StandardCharsets.US_ASCII).trim());
        encKey = Main.hmac(secret, "enc".getBytes(StandardCharsets.US_ASCII));
        macKey = Main.hmac(secret, "mac".getBytes(StandardCharsets.US_ASCII));
        // loads the crypto providers now: done by the first client it costs that client seconds
        reply(new java.io.ByteArrayOutputStream(), res(0, new JSONObject(), null));
        Thread control = new Thread(Guard::control, "guard-control");
        control.setDaemon(true);
        control.start();
        try (ServerSocket ss = new ServerSocket()) {
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress(PORT));
            open(new byte[64]);
            log("in ascolto su " + PORT);
            while (true) {
                Socket c = ss.accept();
                new Thread(() -> serve(c), "guard-client").start();
            }
        }
    }

    private static void control() {
        try (ServerSocket ss = new ServerSocket(CONTROL, 4, java.net.InetAddress.getByName("127.0.0.1"))) {
            while (true) {
                try (Socket c = ss.accept()) {
                    c.setSoTimeout(5000);
                    String cmd = new java.io.BufferedReader(new java.io.InputStreamReader(c.getInputStream(), StandardCharsets.UTF_8)).readLine();
                    c.getOutputStream().write(control(cmd == null ? "" : cmd.trim()).getBytes(StandardCharsets.UTF_8));
                } catch (Exception e) {
                    log("control: " + e);
                }
            }
        } catch (IOException e) {
            log("control: " + e);
        }
    }

    private static String control(String cmd) throws Exception {
        switch (cmd) {
            case "start": sh("rm -f " + DIR + "/stopped"); return "ok\n";
            case "stop":
                log("fermato dall'app");
                sh("touch " + DIR + "/stopped; pkill -f 'dumbphone[.]ponte[.]Main'");
                return "ok\n";
            case "restart": restart("riavvio chiesto dall'app"); return "ok\n";
            case "log": return sh("tail -n 60 " + DIR + "/agent.log");
            default: return "?\n";
        }
    }

    private static void log(String s) {
        System.out.println(new java.text.SimpleDateFormat("MM-dd HH:mm:ss").format(new java.util.Date()) + " " + s);
    }

    private static void serve(Socket c) {
        try {
            c.setTcpNoDelay(true);
            c.setKeepAlive(true);
            // the client pings every 10-20 s: silence means its phone left without closing. The agent then sees the
            // client gone and closes the chat, which would otherwise stay open and mark messages as read.
            c.setSoTimeout(CLIENT_SILENCE_MS);
            Socket a = agent();
            if (a != null) relay(c, a);
            else alone(c);
        } catch (Exception e) {
            log("client: " + e);
        } finally {
            close(c);
        }
    }

    private static Socket agent() {
        Socket a = new Socket();
        try {
            a.connect(new InetSocketAddress("127.0.0.1", AGENT), 1000);
            a.setTcpNoDelay(true);
            return a;
        } catch (IOException e) {
            close(a);
            return null;
        }
    }

    private static boolean agentUp() {
        Socket a = agent();
        close(a);
        return a != null;
    }

    private static void close(Socket s) {
        try { if (s != null) s.close(); } catch (IOException ignored) {}
    }

    /** Agent up: agent→client is a plain copy, client→agent goes frame by frame to spot "restart". */
    private static void relay(Socket c, Socket a) throws Exception {
        java.util.concurrent.atomic.AtomicBoolean answered = new java.util.concurrent.atomic.AtomicBoolean();
        Thread down = new Thread(() -> {
            byte[] b = new byte[65536];
            try {
                InputStream in = a.getInputStream();
                OutputStream out = c.getOutputStream();
                for (int n; (n = in.read(b)) > 0; ) {
                    answered.set(true);
                    out.write(b, 0, n);
                }
            } catch (IOException ignored) {
            } finally {
                close(c);
                close(a);
            }
        }, "guard-down");
        down.start();
        // a hung agent takes the connection but never answers the hello, so the client could not ask for a restart
        Thread hung = new Thread(() -> {
            try {
                Thread.sleep(HUNG_MS);
                long now = System.currentTimeMillis();
                if (!answered.get() && !c.isClosed() && now - lastHung > GAP_MS) {
                    lastHung = now;
                    restart("agente bloccato: nessuna risposta al client");
                    close(c);
                }
            } catch (InterruptedException ignored) {}
        }, "guard-hung");
        hung.setDaemon(true);
        hung.start();
        try {
            DataInputStream in = new DataInputStream(c.getInputStream());
            OutputStream out = a.getOutputStream();
            byte[] head = new byte[4];
            byte[] b = new byte[65536];
            while (true) {
                in.readFully(head);
                int len = length(head);
                if (len <= PEEK_MAX) {
                    byte[] f = new byte[len];
                    in.readFully(f);
                    JSONObject req = open(f);
                    if (req != null && "restart".equals(req.optString("type"))) {
                        reply(c.getOutputStream(), res(req.optLong("id"), new JSONObject(), null));
                        restart("riavvio chiesto dal client");
                        return;
                    }
                    out.write(head);
                    out.write(f);
                } else {
                    out.write(head);
                    for (int left = len; left > 0; ) {
                        int n = in.read(b, 0, Math.min(left, b.length));
                        if (n < 0) throw new IOException("eof");
                        out.write(b, 0, n);
                        left -= n;
                    }
                }
            }
        } catch (IOException e) {
            // either side gone
        } finally {
            close(a);
            close(c);
        }
    }

    /** Agent down: the client still connects and sees why; it is dropped when the agent is back, so it reconnects. */
    private static void alone(Socket c) throws Exception {
        Thread watch = new Thread(() -> {
            try {
                while (!c.isClosed()) {
                    Thread.sleep(2000);
                    if (agentUp()) { close(c); return; }
                }
            } catch (InterruptedException ignored) {}
        }, "guard-watch");
        watch.setDaemon(true);
        watch.start();
        DataInputStream in = new DataInputStream(c.getInputStream());
        OutputStream out = c.getOutputStream();
        byte[] head = new byte[4];
        boolean hello = false;
        try {
            while (true) {
                in.readFully(head);
                int len = length(head);
                if (len > PEEK_MAX) {      // an upload nobody can take now: dropped, the client gets no answer
                    in.readFully(new byte[len]);
                    continue;
                }
                byte[] f = new byte[len];
                in.readFully(f);
                JSONObject req = open(f);
                if (req == null) return;
                String type = req.optString("type");
                long id = req.optLong("id");
                if (!hello) {
                    if (!type.equals("hello")) return;
                    hello = true;
                    log("client " + c.getRemoteSocketAddress() + " con l'agente spento");
                    // same "seq" it sent: it keeps its place and gets the missed events from the agent later
                    reply(out, res(id, state().put("seq", req.optLong("since", -1)).put("resync", false), null));
                    continue;
                }
                switch (type) {
                    case "restart":
                        reply(out, res(id, new JSONObject(), null));
                        restart("riavvio chiesto dal client (agente spento)");
                        break;
                    case "state":
                        reply(out, res(id, state(), null));
                        break;
                    case "ping":
                        reply(out, res(id, new JSONObject(), null));
                        break;
                    case "draft":
                    case "close":
                        break;
                    default:
                        reply(out, res(id, null, "ponte spento: usa riavvia ponte"));
                }
            }
        } catch (java.io.EOFException | java.net.SocketException e) {
            // client gone, or dropped by the watcher
        }
    }

    private static JSONObject state() throws Exception {
        boolean unlocked = "true".equals(sh("getprop sys.user.0.ce_available").trim());
        String why = unlocked ? (new File(DIR, "stopped").exists() ? "fermato: usa riavvia ponte" : "spento: usa riavvia ponte")
                              : "il Pixel va sbloccato una volta";
        return new JSONObject().put("state", "error").put("detail", why).put("open", JSONObject.NULL);
    }

    /** Clears the "stopped" flag; run.sh brings the agent back within seconds (it restarts it whatever way it ended). */
    private static synchronized void restart(String why) {
        log(why);
        try {
            sh("rm -f " + DIR + "/stopped; pkill -f 'dumbphone[.]ponte[.]Main'; sleep 1; "
                    + "pkill -9 -f 'dumbphone[.]ponte[.]Main'");
        } catch (Exception e) {
            log("riavvio: " + e);
        }
    }

    private static String sh(String cmd) throws Exception {
        Process p = new ProcessBuilder("sh", "-c", cmd).redirectErrorStream(true).start();
        byte[] out = p.getInputStream().readAllBytes();
        p.waitFor();
        return new String(out, StandardCharsets.UTF_8);
    }

    private static int length(byte[] h) throws IOException {
        int len = (h[0] & 0xff) | (h[1] & 0xff) << 8 | (h[2] & 0xff) << 16 | (h[3] & 0xff) << 24;
        if (len < 16 + 16 + 32 || len > 64 * 1024 * 1024) throw new IOException("bad frame length " + len);
        return len;
    }

    /** The JSON inside a frame, or null when the mac does not match. */
    private static JSONObject open(byte[] f) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(macKey, "HmacSHA256"));
            mac.update(f, 0, f.length - 32);
            if (!MessageDigest.isEqual(mac.doFinal(), Arrays.copyOfRange(f, f.length - 32, f.length))) return null;
            Cipher d = Cipher.getInstance("AES/CBC/PKCS5Padding");
            d.init(Cipher.DECRYPT_MODE, new SecretKeySpec(encKey, "AES"), new IvParameterSpec(f, 0, 16));
            return new JSONObject(new String(d.doFinal(f, 16, f.length - 48), StandardCharsets.UTF_8));
        } catch (Exception e) {
            return null;
        }
    }

    private static JSONObject res(long id, JSONObject data, String err) throws Exception {
        JSONObject r = new JSONObject().put("type", "res").put("id", id).put("ok", err == null);
        if (err != null) r.put("error", err);
        else r.put("data", data == null ? new JSONObject() : data);
        return r;
    }

    private static void reply(OutputStream out, JSONObject o) throws Exception {
        byte[] iv = new byte[16];
        rnd.nextBytes(iv);
        Cipher e = Cipher.getInstance("AES/CBC/PKCS5Padding");
        e.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(encKey, "AES"), new IvParameterSpec(iv));
        byte[] ct = e.doFinal(o.toString().getBytes(StandardCharsets.UTF_8));
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(macKey, "HmacSHA256"));
        mac.update(iv);
        mac.update(ct);
        byte[] tag = mac.doFinal();
        int len = 16 + ct.length + 32;
        byte[] f = new byte[4 + len];
        f[0] = (byte) len; f[1] = (byte) (len >> 8); f[2] = (byte) (len >> 16); f[3] = (byte) (len >> 24);
        System.arraycopy(iv, 0, f, 4, 16);
        System.arraycopy(ct, 0, f, 20, ct.length);
        System.arraycopy(tag, 0, f, 20 + ct.length, 32);
        out.write(f);
        out.flush();
    }
}
