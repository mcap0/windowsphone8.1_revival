package dumbphone.ponte.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

// The bridge agent seen from the app: is it up, start/stop it through local adb, notifications, the jobs.
final class Ponte {
    static final String DIR = "/data/local/tmp/ponte";
    static final int JOB_BOOT = 1, JOB_WATCH = 2;
    private static final String CHANNEL = "ponte";

    private Ponte() {}

    static boolean running() {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", 8790), 700);   // the console: no "client" lines in the log
            return true;
        } catch (Exception e) {
            android.util.Log.w("Ponte", "running: " + e);
            return false;
        }
    }

    /** The agent's own state line from its console ("ok", "paused ..."), or "" when it does not answer. */
    static String state() {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", 8790), 700);
            s.setSoTimeout(3000);
            PrintWriter out = new PrintWriter(s.getOutputStream(), true);
            BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
            out.print("state\n");
            out.flush();
            String line = in.readLine();
            return line == null ? "" : line;
        } catch (Exception e) {
            return "";
        }
    }

    /** One console command on its own connection (the console serves one client at a time); its whole answer. */
    static String console(String cmd, int timeoutMs) throws Exception {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", 8790), 700);
            s.setSoTimeout(timeoutMs);
            PrintWriter out = new PrintWriter(s.getOutputStream(), true);
            BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8));
            out.print(cmd + "\n");
            out.flush();
            StringBuilder sb = new StringBuilder();
            for (String line; (line = in.readLine()) != null && !line.equals(".");) {
                if (sb.length() > 0) sb.append('\n');
                sb.append(line);
            }
            return sb.toString();
        }
    }

    /** The guard answers on the client port: tailscaled and "riavvia ponte" from the Lumia are up. */
    static boolean guardUp() {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", 8791), 700);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /** One line to the guard's control port (no adb); its answer, or null when the guard does not listen there. */
    private static String control(String cmd) {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", 8792), 700);
            s.setSoTimeout(10000);
            s.getOutputStream().write((cmd + "\n").getBytes(StandardCharsets.UTF_8));
            return new String(s.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        } catch (Exception e) {
            return null;
        }
    }

    private static void waitAgent() throws Exception {
        for (int i = 0; i < 30; i++) {
            if (running()) return;
            Thread.sleep(500);
        }
        throw new Exception("avviato ma non risponde (vedi registro)");
    }

    /** Starts the agent (and the supervisor if needed) and waits for it; throws with the reason. */
    static synchronized void start(Context ctx) throws Exception {
        if (running()) return;
        if (control("start") != null) { waitAgent(); return; }
        run(ctx, "start");
        waitAgent();
    }

    /** Stop and start as one step: the agent is back up, or the reason. */
    static synchronized void restart(Context ctx) throws Exception {
        if (control("restart") != null) {
            Thread.sleep(1500);     // the old agent is going away: do not take it for the new one
            waitAgent();
            return;
        }
        stop(ctx);
        Thread.sleep(1500);
        start(ctx);
    }

    /** Supervisor only (tailscaled + guard), the agent stays as it was: what the job does, also before the first unlock. */
    static synchronized void keep(Context ctx) throws Exception {
        run(ctx, "keep");
        for (int i = 0; i < 30; i++) {
            if (guardUp()) return;
            Thread.sleep(500);
        }
        throw new Exception("supervisore avviato ma la guardia non risponde");
    }

    private static void run(Context ctx, String arg) throws Exception {
        AdbLocal adb = new AdbLocal(ctx);
        try {
            // run.sh supervises agent, guard and tailscaled, restarting each within seconds if it exits
            adb.launch("(setsid nohup sh " + DIR + "/run.sh " + arg + " >/dev/null 2>&1 </dev/null &)");
        } finally {
            adb.close();
        }
    }

    /** Only the agent: tailscaled and the guard stay, so "riavvia ponte" on the Lumia can bring it back. */
    static synchronized void stop(Context ctx) throws Exception {
        if (control("stop") != null) return;
        AdbLocal adb = new AdbLocal(ctx);
        try { adb.shell("sh " + DIR + "/run.sh stop"); } finally { adb.close(); }
    }

    static String log(Context ctx) throws Exception {
        String t = control("log");
        if (t != null) return t;
        AdbLocal adb = new AdbLocal(ctx);
        try { return adb.shell("tail -n 60 " + DIR + "/agent.log"); } finally { adb.close(); }
    }

    /** One try right after boot (once a network is up) plus a check every 15 minutes. */
    static void schedule(Context ctx, boolean boot) {
        JobScheduler js = ctx.getSystemService(JobScheduler.class);
        ComponentName svc = new ComponentName(ctx, WatchJob.class);
        if (boot)
            js.schedule(new JobInfo.Builder(JOB_BOOT, svc).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                    .setOverrideDeadline(10 * 60 * 1000).build());
        if (js.getPendingJob(JOB_WATCH) == null)
            js.schedule(new JobInfo.Builder(JOB_WATCH, svc).setPeriodic(15 * 60 * 1000).setPersisted(true)
                    .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build());
    }

    static void notify(Context ctx, String title, String text, boolean offerStart) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Ponte", NotificationManager.IMPORTANCE_DEFAULT));
        Intent open = new Intent(ctx, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (offerStart) open.putExtra("start", true);
        PendingIntent pi = PendingIntent.getActivity(ctx, offerStart ? 1 : 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        nm.notify(1, new Notification.Builder(ctx, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_upload_done)
                .setContentTitle(title).setContentText(text).setStyle(new Notification.BigTextStyle().bigText(text))
                .setContentIntent(pi).setAutoCancel(true).build());
    }

    static void clearNotification(Context ctx) { ctx.getSystemService(NotificationManager.class).cancel(1); }
}
