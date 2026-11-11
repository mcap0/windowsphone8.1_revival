package dumbphone.pixelfoto;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * tailscaled 1.104.2 (static arm64, shipped in the APK as lib/arm64-v8a/libtailscaled.so) as a child of the service,
 * userspace networking: no VPN slot taken, TCP from the tailnet to this node lands on 127.0.0.1:8792 (PLAN.md §58.3).
 * Node "pixel-foto". The first time `tailscale up` prints a login link: it goes to logcat and to GET /ts (LAN).
 */
final class Ts {
    static final String HOST = "pixel-foto", SOCKET = "@pixelfoto-ts";
    private static final Pattern URL = Pattern.compile("https://login\\.tailscale\\.com/\\S+");

    private final File bin, cli, dir;
    private volatile boolean running;
    private volatile Process daemon;
    private volatile String authUrl, lastLine = "avvio";

    Ts(Context ctx) {
        String lib = ctx.getApplicationInfo().nativeLibraryDir;
        bin = new File(lib, "libtailscaled.so");
        cli = new File(lib, "libtailscale.so");
        dir = new File(ctx.getFilesDir(), "ts");
        dir.mkdirs();
    }

    void start() {
        if (running) return;
        running = true;
        Thread t = new Thread(this::superviseDaemon, "tailscaled");
        t.setDaemon(true);
        t.start();
        Thread u = new Thread(this::upLoop, "tailscale-up");
        u.setDaemon(true);
        u.start();
    }

    void stop() {
        running = false;
        Process p = daemon;
        if (p != null) p.destroy();
    }

    /** tailscaled, restarted 10 s after it exits. */
    private void superviseDaemon() {
        while (running) {
            try {
                ProcessBuilder pb = new ProcessBuilder(bin.getPath(), "--no-logs-no-support", "--tun=userspace-networking",
                        "--statedir=" + new File(dir, "state").getPath(), "--socket=" + SOCKET, "--port=41641");
                pb.environment().put("HOME", dir.getPath());
                pb.redirectErrorStream(true);
                pb.redirectOutput(new File(dir, "tailscaled.log"));
                Process p = pb.start();
                daemon = p;
                Log.i("tailscaled avviato");
                int code = p.waitFor();
                Log.i("tailscaled uscito (" + code + ")");
            } catch (IOException e) {
                Log.i("tailscaled: " + e);
            } catch (InterruptedException e) {
                return;
            }
            sleep(10_000);
        }
    }

    /** `tailscale up` until the node is logged in; then every 10 min a status check that redoes it if needed. */
    private void upLoop() {
        sleep(4000);
        while (running) {
            String st = run(Arrays.asList("status"), 20_000);
            boolean ok = st != null && st.contains(HOST) && !st.contains("Logged out") && !st.contains("NeedsLogin")
                    && !st.contains("stopped");
            if (!ok) {
                lastLine = "login…";
                String out = run(Arrays.asList("up", "--hostname=" + HOST, "--accept-dns=false", "--timeout=10m"), 11 * 60_000);
                Log.i("tailscale up: " + (out == null ? "nessuna risposta" : out.trim().replace('\n', ' ')));
            } else {
                authUrl = null;
                lastLine = "connesso";
            }
            sleep(ok ? 600_000 : 15_000);
        }
    }

    /** Runs the CLI against our tailscaled; output (stdout+stderr), or null. Login links are picked up on the fly. */
    private String run(List<String> args, long timeoutMs) {
        try {
            List<String> cmd = new java.util.ArrayList<>();
            cmd.add(cli.getPath());
            cmd.add("--socket=" + SOCKET);
            cmd.addAll(args);
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.environment().put("HOME", dir.getPath());
            pb.redirectErrorStream(true);
            Process p = pb.start();
            StringBuilder sb = new StringBuilder();
            Thread reader = new Thread(() -> {
                try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = r.readLine()) != null) {
                        synchronized (sb) { sb.append(line).append('\n'); }
                        Matcher m = URL.matcher(line);
                        if (m.find() && !m.group().equals(authUrl)) {
                            authUrl = m.group();
                            Log.i("tailscale: serve il login " + authUrl);
                        }
                    }
                } catch (IOException ignored) { }
            });
            reader.setDaemon(true);
            reader.start();
            if (!p.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)) p.destroy();
            reader.join(2000);
            synchronized (sb) { return sb.toString(); }
        } catch (IOException | InterruptedException e) {
            Log.i("tailscale " + args + ": " + e);
            return null;
        }
    }

    /** For GET /ts: state, login link if one is pending, `tailscale status`. */
    String report() {
        StringBuilder sb = new StringBuilder();
        sb.append("stato: ").append(lastLine).append('\n');
        if (authUrl != null) sb.append("login: ").append(authUrl).append('\n');
        String st = run(Arrays.asList("status"), 10_000);
        sb.append(st == null ? "(status non disponibile)" : st);
        return sb.toString();
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException ignored) { }
    }
}
