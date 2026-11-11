package dumbphone.ponte.app;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.provider.Settings;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.Principal;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;

import javax.net.ssl.KeyManager;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509KeyManager;
import javax.net.ssl.X509TrustManager;

// adb from the phone to itself: turns on wireless debugging (WRITE_SECURE_SETTINGS, granted once with pm grant),
// finds adbd's TLS port over mDNS and speaks the adb protocol with the app's own key (assets/adbkey.der, authorized
// once like a PC over USB). Commands run as the shell user, which is what the bridge agent needs.
final class AdbLocal {
    private static final int CNXN = 0x4e584e43, OPEN = 0x4e45504f, OKAY = 0x59414b4f, CLSE = 0x45534c43,
            WRTE = 0x45545257, STLS = 0x534c5453;

    private final Context ctx;

    AdbLocal(Context ctx) { this.ctx = ctx.getApplicationContext(); }

    /** Runs a shell command and returns its output; throws with a readable reason when adb is not reachable. */
    String shell(String cmd) throws Exception { return run("shell:" + cmd, null); }

    /** Starts a detached command: the connection must stay open while it starts (closing it early kills it). */
    void launch(String cmd) throws Exception { run("exec:" + cmd + "; sleep 8", null); }

    private String run(String service, java.util.function.BooleanSupplier ready) throws Exception {
        int port = port();
        try (Socket raw = new Socket()) {
            raw.connect(new InetSocketAddress("127.0.0.1", port), 3000);
            raw.setSoTimeout(15000);
            send(raw.getOutputStream(), CNXN, 0x01000001, 1024 * 1024, "host::\0".getBytes(StandardCharsets.UTF_8));
            int[] h = new int[6];
            read(new DataInputStream(raw.getInputStream()), h);
            if (h[0] != STLS) throw new IOException("adbd non chiede TLS");
            send(raw.getOutputStream(), STLS, 0x01000000, 0, new byte[0]);
            SSLSocket s = (SSLSocket) tls().getSocketFactory().createSocket(raw, "127.0.0.1", port, true);
            s.setUseClientMode(true);
            // with TLS 1.3 the handshake ends on our side before adbd checks our certificate: a refused key shows up
            // on the first read, as CERTIFICATE_UNKNOWN
            byte[] data;
            DataInputStream in;
            OutputStream out;
            try {
                s.startHandshake();
                in = new DataInputStream(s.getInputStream());
                out = s.getOutputStream();
                data = read(in, h);
            } catch (javax.net.ssl.SSLException e) {
                throw new IOException("chiave adb dell'app non autorizzata: da PC, USB, ponte/app/keys.sh (" + e.getMessage() + ")");
            }
            if (h[0] != CNXN) throw new IOException("adbd: risposta inattesa");
            send(out, OPEN, 1, 0, (service + "\0").getBytes(StandardCharsets.UTF_8));
            if (ready != null) {
                for (int i = 0; i < 30 && !ready.getAsBoolean(); i++) Thread.sleep(500);
                return "";
            }
            ByteArrayOutputStream res = new ByteArrayOutputStream();
            while (true) {
                data = read(in, h);
                if (h[0] == WRTE) {
                    res.write(data);
                    send(out, OKAY, 1, h[1], new byte[0]);
                } else if (h[0] == CLSE) {
                    send(out, CLSE, 1, h[1], new byte[0]);
                    break;
                }
            }
            return res.toString("UTF-8");
        }
    }

    /** Wireless debugging is only a door for starting/stopping the agent: closed again right after. */
    void close() {
        try { Settings.Global.putInt(ctx.getContentResolver(), "adb_wifi_enabled", 0); } catch (Exception ignored) {}
    }

    /** adbd's TLS port: wireless debugging on (works silently on a network already allowed "always"), then mDNS. */
    private int port() throws Exception {
        if (Settings.Global.getInt(ctx.getContentResolver(), "adb_wifi_enabled", 0) != 1) {
            try {
                Settings.Global.putInt(ctx.getContentResolver(), "adb_wifi_enabled", 1);
            } catch (SecurityException e) {
                throw new IOException("manca WRITE_SECURE_SETTINGS (pm grant dal PC)");
            }
        }
        for (int i = 0; i < 4; i++) {
            // mDNS also hands back ports of earlier sessions (cached): take the one adbd really listens on
            for (int p : discover()) {
                try (Socket t = new Socket()) {
                    t.connect(new InetSocketAddress("127.0.0.1", p), 1000);
                    return p;
                } catch (IOException ignored) {}
            }
            if (Settings.Global.getInt(ctx.getContentResolver(), "adb_wifi_enabled", 0) != 1)
                throw new IOException("debug wireless spento: serve il Wi-Fi di casa (rete consentita)");
        }
        throw new IOException("debug wireless: porta non trovata");
    }

    private java.util.List<Integer> discover() throws InterruptedException {
        NsdManager nsd = (NsdManager) ctx.getSystemService(Context.NSD_SERVICE);
        java.util.List<Integer> ports = java.util.Collections.synchronizedList(new java.util.ArrayList<>());
        NsdManager.DiscoveryListener l = new NsdManager.DiscoveryListener() {
            public void onStartDiscoveryFailed(String t, int e) {}
            public void onStopDiscoveryFailed(String t, int e) {}
            public void onDiscoveryStarted(String t) {}
            public void onDiscoveryStopped(String t) {}
            public void onServiceLost(NsdServiceInfo i) {}
            public void onServiceFound(NsdServiceInfo info) {
                nsd.resolveService(info, new NsdManager.ResolveListener() {
                    public void onResolveFailed(NsdServiceInfo i, int e) {}
                    public void onServiceResolved(NsdServiceInfo i) {
                        if (i.getHost() != null && isMine(i.getHost()) && !ports.contains(i.getPort())) ports.add(0, i.getPort());
                    }
                });
            }
        };
        nsd.discoverServices("_adb-tls-connect._tcp", NsdManager.PROTOCOL_DNS_SD, l);
        Thread.sleep(4000);
        try { nsd.stopServiceDiscovery(l); } catch (Exception ignored) {}
        return new java.util.ArrayList<>(ports);
    }

    private static boolean isMine(java.net.InetAddress a) {
        try {
            return a.isLoopbackAddress() || java.net.NetworkInterface.getByInetAddress(a) != null;
        } catch (Exception e) {
            return false;
        }
    }

    private SSLContext tls() throws Exception {
        PrivateKey key = KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(asset("adbkey.der")));
        X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(new java.io.ByteArrayInputStream(asset("adbcert.der")));
        X509KeyManager km = new X509KeyManager() {
            public String[] getClientAliases(String t, Principal[] p) { return new String[]{"k"}; }
            public String chooseClientAlias(String[] t, Principal[] p, Socket s) { return "k"; }
            public String[] getServerAliases(String t, Principal[] p) { return null; }
            public String chooseServerAlias(String t, Principal[] p, Socket s) { return null; }
            public X509Certificate[] getCertificateChain(String a) { return new X509Certificate[]{cert}; }
            public PrivateKey getPrivateKey(String a) { return key; }
        };
        X509TrustManager tm = new X509TrustManager() {
            public void checkClientTrusted(X509Certificate[] c, String a) {}
            public void checkServerTrusted(X509Certificate[] c, String a) {}   // adbd on this same phone
            public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
        };
        SSLContext c = SSLContext.getInstance("TLSv1.3");
        c.init(new KeyManager[]{km}, new TrustManager[]{tm}, null);
        return c;
    }

    private byte[] asset(String name) throws IOException {
        try (InputStream in = ctx.getAssets().open(name)) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
            return b.toByteArray();
        }
    }

    private static void send(OutputStream out, int cmd, int a0, int a1, byte[] data) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(24 + data.length).order(ByteOrder.LITTLE_ENDIAN);
        int crc = 0;
        for (byte x : data) crc += x & 0xff;
        b.putInt(cmd).putInt(a0).putInt(a1).putInt(data.length).putInt(crc).putInt(~cmd).put(data);
        out.write(b.array());
        out.flush();
    }

    private static byte[] read(DataInputStream in, int[] h) throws IOException {
        byte[] head = new byte[24];
        in.readFully(head);
        ByteBuffer b = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < 6; i++) h[i] = b.getInt();
        byte[] data = new byte[h[3]];
        in.readFully(data);
        return data;
    }
}
