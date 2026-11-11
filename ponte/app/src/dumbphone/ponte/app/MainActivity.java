package dumbphone.ponte.app;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

// Status of the bridge, start/stop/restart, the agent's log.
public final class MainActivity extends Activity {
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status, log;
    private boolean busy;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (checkSelfPermission("android.permission.POST_NOTIFICATIONS") != 0)
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 1);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad * 3, pad, pad);
        status = new TextView(this);
        status.setTextSize(20);
        root.addView(status);
        LinearLayout row = new LinearLayout(this);
        row.addView(button("Avvia", () -> Ponte.start(this)));
        row.addView(button("Ferma", () -> Ponte.stop(this)));
        row.addView(button("Riavvia", () -> Ponte.restart(this)));
        row.addView(button("Registro", () -> { String t = Ponte.log(this); ui.post(() -> log.setText(t)); }));
        root.addView(row);
        log = new TextView(this);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(11);
        ScrollView sv = new ScrollView(this);
        sv.addView(log);
        root.addView(sv);
        setContentView(root);
        Ponte.schedule(this, false);
        if (getIntent().getBooleanExtra("start", false)) act(() -> Ponte.start(this));
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private interface Op { void run() throws Exception; }

    private Button button(String text, Op op) {
        Button b = new Button(this);
        b.setText(text);
        b.setOnClickListener(v -> act(op));
        return b;
    }

    private void act(Op op) {
        if (busy) return;       // one operation at a time: a second press while one runs is dropped
        busy = true;
        status.setText("…");
        new Thread(() -> {
            String err = null;
            try {
                op.run();
            } catch (Exception e) {
                err = e.getMessage();
            }
            String e2 = err;
            ui.post(() -> {
                busy = false;
                refresh();
                if (e2 != null) log.setText("Errore: " + e2);
            });
        }).start();
    }

    private void refresh() {
        new Thread(() -> {
            boolean up = Ponte.running(), guard = Ponte.guardUp();
            String st = up ? Ponte.state() : "";
            if (up) Ponte.clearNotification(this);
            ui.post(() -> status.setText(up ? "Ponte attivo\n" + st : guard ? "Ponte spento, Tailscale attivo" : "Ponte spento"));
        }).start();
    }
}
