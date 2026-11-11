package dumbphone.ponte;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

// Remoto's side of the "Tastiera del Lumia" (ponte/app RemotoIme, PLAN §68). For a session the Pixel's default
// keyboard becomes RemotoIme and Remoto's display shows keyboards locally (DISPLAY_IME_POLICY_LOCAL): an app on it
// then shrinks above the keyboard strip as on a phone. After the session: the previous keyboard and policy HIDE
// again. The keyboard connects to 127.0.0.1:8797; only the Ponte app's uid is accepted (/proc/net/tcp has the uid
// of every socket). Its link format: kind byte + u32 LE length + body (see RemotoIme).
final class RemotoKeys {
    static final String IME = "dumbphone.ponte.app/.RemotoIme";
    static final int PORT = 8797;
    private static final String SAVED = Main.DIR + "/remoto.ime";   // the keyboard to give back (also after a crash)

    interface Client { void send(char kind, byte[] body); }   // to the Lumia of the current session

    private static int displayId;
    private static volatile Client client;
    private static volatile OutputStream ime;
    private static volatile boolean field;     // the keyboard says a field on our display wants input
    private static final ExecutorService switcher = Executors.newSingleThreadExecutor();   // in order, never on a reader

    static void start(int display) {
        displayId = display;
        // a crash during a session left RemotoIme as the keyboard (no keys: the Pixel's own screen could not type)
        switcher.execute(() -> restore("avvio"));
        Thread t = new Thread(RemotoKeys::serve, "ponte-remoto-ime");
        t.setDaemon(true);
        t.start();
    }

    static void sessionStart(Client c) {
        client = c;
        switcher.execute(() -> {
            try {
                String current = Main.sh("settings get secure default_input_method").trim();
                if (!current.isEmpty() && !current.equals("null") && !current.equals(IME))
                    Files.write(Paths.get(SAVED), current.getBytes(StandardCharsets.UTF_8));
                Main.sh("ime enable " + IME + " >/dev/null; ime set " + IME + " >/dev/null");
                policy(0);   // DISPLAY_IME_POLICY_LOCAL
                Main.log("remoto: tastiera del Lumia attiva (prima: " + current + ")");
            } catch (Exception e) {
                Main.log("remoto: tastiera " + e);
            }
        });
    }

    static void sessionEnd(Client c) {
        if (client != c) return;   // a newer session took over: its keyboard stays
        client = null;
        field = false;
        switcher.execute(() -> {
            if (client != null) return;
            restore("fine sessione");
        });
    }

    private static void restore(String why) {
        try {
            policy(2);   // DISPLAY_IME_POLICY_HIDE: with no Lumia nobody types here (VDisplay.hideIme)
            String current = Main.sh("settings get secure default_input_method").trim();
            if (!current.equals(IME)) return;
            String back = "com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME";
            try { back = new String(Files.readAllBytes(Paths.get(SAVED)), StandardCharsets.UTF_8).trim(); } catch (IOException ignored) {}
            Main.sh("ime set " + back + " >/dev/null");
            Main.log("remoto: tastiera rimessa (" + why + "): " + back);
        } catch (Exception e) {
            Main.log("remoto: tastiera " + e);
        }
    }

    private static void policy(int p) throws Exception {
        Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
        Object wms = wmg.getMethod("getWindowManagerService").invoke(null);
        wms.getClass().getMethod("setDisplayImePolicy", int.class, int.class).invoke(wms, displayId, p);
    }

    // --------------------------------------------------------------------------------------- the Lumia's input

    /** Text from the Lumia: committed by the keyboard when a field is open; false = use the key events path. */
    static boolean text(String s) {
        return field && toIme('X', s.getBytes(StandardCharsets.UTF_8));
    }

    static boolean key(int code) {
        if (!field || (code != 67 && code != 66)) return false;   // KEYCODE_DEL, KEYCODE_ENTER
        return toIme('B', new byte[] {(byte) code, (byte) (code >> 8)});
    }

    /** The Lumia's keyboard height in display pixels, 0 = it went down. */
    static void height(int px) {
        toIme('h', new byte[] {(byte) px, (byte) (px >> 8)});
        if (px == 0) field = false;
    }

    private static synchronized boolean toIme(char kind, byte[] body) {
        OutputStream o = ime;
        if (o == null) return false;
        byte[] d = new byte[5 + body.length];
        d[0] = (byte) kind;
        int n = body.length;
        d[1] = (byte) n; d[2] = (byte) (n >> 8); d[3] = (byte) (n >> 16); d[4] = (byte) (n >> 24);
        System.arraycopy(body, 0, d, 5, n);
        try {
            o.write(d);
            o.flush();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // --------------------------------------------------------------------------------------- the keyboard's link

    private static void serve() {
        int uid = -1;
        try (ServerSocket ss = new ServerSocket(PORT, 2, InetAddress.getByName("127.0.0.1"))) {
            while (true) {
                Socket s = ss.accept();
                if (uid < 0) uid = appUid();
                int peer = peerUid(s.getPort());
                if (uid < 0 || peer != uid) {
                    Main.log("remoto: tastiera: rifiutato uid " + peer + " (Ponte è " + uid + ")");
                    s.close();
                    continue;
                }
                Main.log("remoto: tastiera collegata (uid " + peer + ")");
                Thread t = new Thread(() -> link(s), "ponte-remoto-ime-link");
                t.setDaemon(true);
                t.start();
            }
        } catch (Exception e) {
            Main.log("remoto: tastiera: server " + e);
        }
    }

    private static void link(Socket s) {
        try {
            s.setTcpNoDelay(true);
            synchronized (RemotoKeys.class) { ime = s.getOutputStream(); }
            toIme('d', new byte[] {(byte) displayId, (byte) (displayId >> 8), (byte) (displayId >> 16), (byte) (displayId >> 24)});
            DataInputStream in = new DataInputStream(s.getInputStream());
            byte[] len = new byte[4];
            while (true) {
                int kind = in.readUnsignedByte();
                in.readFully(len);
                int n = (len[0] & 0xFF) | (len[1] & 0xFF) << 8 | (len[2] & 0xFF) << 16 | (len[3] & 0xFF) << 24;
                if (n < 0 || n > (1 << 20)) break;
                byte[] body = new byte[n];
                in.readFully(body);
                Client c = client;
                if (kind == 'I') {
                    String json = new String(body, StandardCharsets.UTF_8);
                    Main.log("remoto: campo " + json);
                    field = json.contains("\"on\":true");
                    if (c != null) c.send('I', body);
                } else if (kind == 'c') {
                    Main.log("remoto: copiato sul Pixel, " + n + " byte al Lumia");
                    if (c != null) c.send('c', body);
                }
            }
        } catch (Exception e) {
            // the keyboard went away (switched off after the session, app updated)
        } finally {
            synchronized (RemotoKeys.class) {
                try { if (ime == s.getOutputStream()) { ime = null; field = false; } } catch (IOException ignored) { ime = null; }
            }
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    private static int appUid() {
        try {
            String out = Main.sh("pm list packages -U dumbphone.ponte.app");
            int at = out.indexOf("uid:");
            if (at >= 0) return Integer.parseInt(out.substring(at + 4).trim().split("[^0-9]")[0]);
        } catch (Exception e) {
            Main.log("remoto: tastiera: uid " + e);
        }
        return -1;
    }

    // The uid of the socket whose local port is the peer's port and remote port ours (the keyboard's end).
    private static int peerUid(int port) {
        String local = String.format(":%04X", port), remote = String.format(":%04X", PORT);
        for (String file : new String[] {"/proc/net/tcp6", "/proc/net/tcp"}) {
            try {
                for (String line : Files.readAllLines(Paths.get(file))) {
                    String[] f = line.trim().split("\\s+");
                    if (f.length > 7 && f[1].endsWith(local) && f[2].endsWith(remote)) return Integer.parseInt(f[7]);
                }
            } catch (Exception ignored) {}
        }
        return -2;
    }

    private RemotoKeys() {}
}
