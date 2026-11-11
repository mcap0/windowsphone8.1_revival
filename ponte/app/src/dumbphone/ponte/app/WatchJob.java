package dumbphone.ponte.app;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

// After boot (already before the first unlock) and every 15 minutes: keep the supervisor up. It brings up tailscaled,
// the guard and, unless stopped by hand or still locked, the agent; everything else is up to it.
public final class WatchJob extends JobService {
    @Override
    public boolean onStartJob(JobParameters p) {
        boolean boot = p.getJobId() == Ponte.JOB_BOOT;
        new Thread(() -> {
            if (!Ponte.guardUp()) {
                try {
                    Ponte.keep(this);
                    Ponte.notify(this, boot ? "Ponte riavviato" : "Ponte ripartito",
                            boot ? "Tailscale attivo dopo il riavvio." : "Il supervisore era caduto: riavviato.", false);
                } catch (Exception e) {
                    Ponte.notify(this, "Ponte spento: tocca per avviarlo", String.valueOf(e.getMessage()), true);
                }
            }
            jobFinished(p, false);
        }, "ponte-job").start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters p) { return false; }

    public static final class Boot extends BroadcastReceiver {
        @Override
        public void onReceive(Context ctx, Intent i) {
            Ponte.schedule(ctx, true);
        }
    }
}
