package dumbphone.ponte.app;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Display;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

// "Tastiera del Lumia": the Pixel's keyboard while the Lumia uses Remoto (PLAN §68). Remoto (ponte/agent Remoto.java)
// makes it the default keyboard when a session starts and puts the previous one back when it ends. It draws no keys:
// the Lumia's own keyboard types. What it does, as only a keyboard can:
//  - it knows when a text field on Remoto's display asks for a keyboard (onStartInputView) and what kind of field it
//    is (text, number, e-mail, search, send...): the Lumia opens its keyboard by itself, with the right layout;
//  - its window is an empty black strip as tall as the Lumia's keyboard, on Remoto's display: the app shrinks above
//    it as with a real keyboard, so the field is never hidden behind the Lumia's keyboard;
//  - text goes in with commitText (accents, emoji: no clipboard trick), Enter does the field's action (Send, Search);
//  - as the default keyboard it may read the clipboard: what is copied on the Pixel goes to the Lumia.
// Link: TCP 127.0.0.1:8797 to Remoto, which accepts only this app's uid (it reads /proc/net/tcp). Messages are
// kind byte + u32 little-endian length + body, both ways:
//   Remoto -> here  'd' u32 Remoto's display id | 'X' UTF-8 text to commit | 'B' u16 keycode (DEL, ENTER...)
//                   'h' u16 strip height in display pixels (0 = the Lumia's keyboard is down: hide)
//   here -> Remoto  'I' JSON {"on":bool,"display":id,"type":...,"action":...,"multi":bool} | 'c' UTF-8 copied text
public final class RemotoIme extends InputMethodService {
    static final int PORT = 8797;
    static final String OWN_CLIP = "remoto";   // Remoto's own clips (the old paste path): not sent back

    private Handler main;
    private volatile boolean alive;
    private volatile Socket socket;
    private OutputStream out;
    // Android re-creates this service each time its window moves to another display: kept across instances
    private static int remotoDisplay = -1;
    private static int height;      // px; kept across instances: the next field opens at the right size at once
    private View strip;
    private boolean active;         // an input view is up on Remoto's display
    private EditorInfo editor;
    private ClipboardManager clipboard;
    private final ClipboardManager.OnPrimaryClipChangedListener clipListener = this::clipChanged;

    @Override public void onCreate() {
        super.onCreate();
        main = new Handler(Looper.getMainLooper());
        alive = true;
        android.util.Log.i("RemotoIme", "creata");
        clipboard = getSystemService(ClipboardManager.class);
        clipboard.addPrimaryClipChangedListener(clipListener);
        Thread t = new Thread(this::link, "remoto-ime-link");
        t.setDaemon(true);
        t.start();
    }

    @Override public void onDestroy() {
        alive = false;
        android.util.Log.i("RemotoIme", "distrutta");
        clipboard.removePrimaryClipChangedListener(clipListener);
        super.onDestroy();   // may still call onFinishInputView, which sends
        writer.shutdown();
        Socket s = socket;
        if (s != null) try { s.close(); } catch (IOException ignored) {}
    }

    // ------------------------------------------------------------------------------------------- the strip

    @Override public View onCreateInputView() {
        strip = new View(this) {
            @Override protected void onMeasure(int w, int h) {
                setMeasuredDimension(View.MeasureSpec.getSize(w), height);
            }
        };
        strip.setBackgroundColor(0xFF000000);
        return strip;
    }

    @Override public boolean onEvaluateFullscreenMode() { return false; }
    @Override public boolean onEvaluateInputViewShown() { return true; }   // also with a "hardware" keyboard

    private int windowDisplay() {
        try {
            Display d = getWindow().getWindow().getDecorView().getDisplay();
            if (d != null) return d.getDisplayId();
        } catch (Exception ignored) {}
        try { return getDisplay().getDisplayId(); } catch (Exception e) { return -1; }
    }

    @Override public void onStartInputView(EditorInfo info, boolean restarting) {
        super.onStartInputView(info, restarting);
        editor = info;
        evaluate();
    }

    // A field asks for the keyboard: tell the Lumia if it is on Remoto's display. Called again when the link comes
    // back (the new instance after a display move may get the field before it has reconnected).
    private void evaluate() {
        EditorInfo info = editor;
        if (info == null) return;   // set only between onStartInputView and onFinishInputView
        int display = windowDisplay();
        if (socket == null || remotoDisplay < 0) return;   // the link's 'd' calls this again
        if (display != remotoDisplay) {
            // a field on the Pixel's own screen (or Remoto is gone): this keyboard has no keys, step aside
            active = false;
            android.util.Log.i("RemotoIme", "campo sul display " + display + ", Remoto è " + remotoDisplay);
            requestHideSelf(0);
            return;
        }
        active = true;
        send('I', describe(info, true, display).getBytes(StandardCharsets.UTF_8));
    }

    @Override public void onWindowShown() {
        super.onWindowShown();
        if (!active) evaluate();
    }

    @Override public void onFinishInputView(boolean finishingInput) {
        super.onFinishInputView(finishingInput);
        editor = null;
        if (active) {
            active = false;
            send('I', "{\"on\":false}".getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String describe(EditorInfo info, boolean on, int display) {
        try {
            int cls = info.inputType & InputType.TYPE_MASK_CLASS, var = info.inputType & InputType.TYPE_MASK_VARIATION;
            String type = "text";
            if (cls == InputType.TYPE_CLASS_NUMBER) type = var == InputType.TYPE_NUMBER_VARIATION_PASSWORD ? "pin" : "number";
            else if (cls == InputType.TYPE_CLASS_PHONE) type = "phone";
            else if (cls == InputType.TYPE_CLASS_TEXT) {
                if (var == InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS || var == InputType.TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS) type = "email";
                else if (var == InputType.TYPE_TEXT_VARIATION_URI) type = "url";
                else if (var == InputType.TYPE_TEXT_VARIATION_PASSWORD || var == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD
                        || var == InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD) type = "password";
            }
            String action;
            switch (info.imeOptions & EditorInfo.IME_MASK_ACTION) {
                case EditorInfo.IME_ACTION_SEARCH: action = "search"; break;
                case EditorInfo.IME_ACTION_SEND: action = "send"; break;
                case EditorInfo.IME_ACTION_GO: action = "go"; break;
                case EditorInfo.IME_ACTION_NEXT: action = "next"; break;
                case EditorInfo.IME_ACTION_DONE: action = "done"; break;
                default: action = "none";
            }
            return new JSONObject().put("on", on).put("display", display).put("type", type).put("action", action)
                    .put("multi", (info.inputType & InputType.TYPE_TEXT_FLAG_MULTI_LINE) != 0)
                    .put("package", String.valueOf(info.packageName)).toString();
        } catch (Exception e) {
            return "{\"on\":" + on + "}";
        }
    }

    // ------------------------------------------------------------------------------------------- typing

    private void commit(String text) {
        InputConnection ic = getCurrentInputConnection();
        if (ic == null) return;
        ic.beginBatchEdit();
        // a line break inside pasted text is a new line in a multi-line field, the field's Enter otherwise
        String[] lines = text.split("\n", -1);
        for (int i = 0; i < lines.length; i++) {
            if (i > 0) enter(ic);
            if (!lines[i].isEmpty()) ic.commitText(lines[i], 1);
        }
        ic.endBatchEdit();
    }

    private void enter(InputConnection ic) {
        EditorInfo info = editor;
        boolean multi = info != null && (info.inputType & InputType.TYPE_TEXT_FLAG_MULTI_LINE) != 0;
        int action = info == null ? EditorInfo.IME_ACTION_UNSPECIFIED : info.imeOptions & EditorInfo.IME_MASK_ACTION;
        boolean noAction = info == null || (info.imeOptions & EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0;
        if (!multi && !noAction && action != EditorInfo.IME_ACTION_NONE && action != EditorInfo.IME_ACTION_UNSPECIFIED)
            ic.performEditorAction(action);
        else
            sendDownUpKeyEvents(KeyEvent.KEYCODE_ENTER);
    }

    private void key(int code) {
        InputConnection ic = getCurrentInputConnection();
        if (ic == null) return;
        if (code == KeyEvent.KEYCODE_ENTER) enter(ic);
        else sendDownUpKeyEvents(code);   // DEL: the field deletes the selection or one character, as it likes
    }

    private void setHeight(int px) {
        height = Math.max(0, Math.min(px, 1600));
        android.util.Log.i("RemotoIme", "altezza " + px + ", striscia " + (strip != null) + ", display " + windowDisplay());
        if (px == 0) {
            requestHideSelf(0);   // the Lumia's keyboard went down (Back): so does this one; a tap on a field brings it back
            return;
        }
        if (strip != null) strip.requestLayout();
    }

    // ------------------------------------------------------------------------------------------- clipboard

    private String lastClip;
    private long lastClipAt;

    private void clipChanged() {
        try {
            ClipData clip = clipboard.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) return;
            CharSequence label = clip.getDescription().getLabel();
            if (label != null && OWN_CLIP.contentEquals(label)) return;
            CharSequence text = clip.getItemAt(0).coerceToText(this);
            if (text == null || text.length() == 0) return;
            String s = text.toString();
            if (s.length() > 32768) s = s.substring(0, 32768);
            long now = android.os.SystemClock.uptimeMillis();
            if (s.equals(lastClip) && now - lastClipAt < 2000) return;   // Android reports one copy twice
            lastClip = s;
            lastClipAt = now;
            send('c', s.getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            android.util.Log.w("RemotoIme", "appunti: " + e);
        }
    }

    // ------------------------------------------------------------------------------------------- link

    // Writes go through one thread of their own: the callbacks run on the main thread, where Android forbids I/O.
    private final java.util.concurrent.ExecutorService writer = java.util.concurrent.Executors.newSingleThreadExecutor();

    private void send(char kind, byte[] body) {
        byte[] d = new byte[5 + body.length];
        d[0] = (byte) kind;
        int n = body.length;
        d[1] = (byte) n; d[2] = (byte) (n >> 8); d[3] = (byte) (n >> 16); d[4] = (byte) (n >> 24);
        System.arraycopy(body, 0, d, 5, n);
        if (writer.isShutdown()) return;
        writer.execute(() -> {
            OutputStream o;
            synchronized (this) { o = out; }
            if (o == null) return;
            try {
                o.write(d);
                o.flush();
            } catch (IOException e) {
                Socket s = socket;
                if (s != null) try { s.close(); } catch (IOException ignored) {}
            }
        });
    }

    private void link() {
        while (alive) {
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress("127.0.0.1", PORT), 1000);
                s.setTcpNoDelay(true);
                synchronized (this) { out = s.getOutputStream(); }
                socket = s;
                DataInputStream in = new DataInputStream(s.getInputStream());
                byte[] len = new byte[4];
                while (alive) {
                    int kind = in.readUnsignedByte();
                    in.readFully(len);
                    int n = (len[0] & 0xFF) | (len[1] & 0xFF) << 8 | (len[2] & 0xFF) << 16 | (len[3] & 0xFF) << 24;
                    if (n < 0 || n > (1 << 20)) break;
                    byte[] body = new byte[n];
                    in.readFully(body);
                    main.post(() -> message(kind, body));
                }
            } catch (Exception e) {
                // Remoto not up (no session yet, or restarting): try again
            } finally {
                synchronized (this) { out = null; }
                socket = null;
            }
            if (alive) android.os.SystemClock.sleep(1000);
        }
    }

    private void message(int kind, byte[] body) {
        switch (kind) {
            case 'd': remotoDisplay = le(body, 0, 4); if (!active) evaluate(); break;
            case 'X': commit(new String(body, StandardCharsets.UTF_8)); break;
            case 'B': key(le(body, 0, 2)); break;
            case 'h': setHeight(le(body, 0, 2)); break;
            default: break;
        }
    }

    private static int le(byte[] b, int at, int n) {
        int v = 0;
        for (int i = 0; i < n && at + i < b.length; i++) v |= (b[at + i] & 0xFF) << (8 * i);
        return v;
    }
}
