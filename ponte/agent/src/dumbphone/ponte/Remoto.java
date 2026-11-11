package dumbphone.ponte;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.hardware.input.InputManager;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import android.view.Surface;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.List;

// Remoto, Pixel side (PLAN §64.3): any app of this phone on the Lumia. A virtual display "remoto" at the Lumia's size
// feeds a MediaCodec H.264 encoder; access units go to the Lumia's Remoto app, touches and keys come back and are
// injected on that display. Its own process (run.sh keeps it up), so restarting other Ponte processes never cuts it.
//   CLASSPATH=/data/local/tmp/ponte/agent.dex app_process / dumbphone.ponte.Remoto
//
// The display lives as long as the process (its apps keep their state between sessions). With no client it has no
// surface and no wake lock: nothing is composed or encoded and its apps stop as with the screen off. Each client gets
// a new RemotoStream (GL + encoder + pacer), so its first frame is a key frame.
//
// Protocol (TCP 8795, LAN and tailnet; little endian), the one of tools/remotobench plus auth and input:
//   server -> client  'C' 16-byte nonce                         (first; the client answers 'A')
//                     'V' u32 length, u64 server_us, u32 seq, Annex-B access unit (key frames start with SPS)
//                     'P' u64 server_us, u64 echoed client_us
//                     'U' u32 length, u64 server_us, Opus packet (48 kHz stereo, 20 ms; the Pixel's sound output)
//                     'a' u32 length, JSON [{"p":package,"l":label}]   (answer to 'Q')
//                     'o' u32 length, JSON [{"t":root task id,"p":package,"l":label}], front first (answer to 'q')
//   client -> server  'A' HMAC-SHA256(mac key of the ponte key, nonce) (32 bytes; anything else = closed)
//                     'p' u64 client_us | 'S' u32 length, text (logged) | 'K' (key frame now)
//                     'T' u8 action (0 down, 1 up, 2 move, 3 cancel), u8 pointer id, u16 x, u16 y (display pixels)
//                     'B' u16 Android keycode (down + up)  | 'X' u32 length, UTF-8 text typed as key events
//                     'L' u32 length, package (start its launcher activity here) | 'R' same, force-stopped first
//                     'Q' (list launchable apps -> 'a')
//                     'q' (list the apps open on the display -> 'o') | 'x' u32 root task id (close it; 0 = all of them)
//                     'W' u8 rotation 0-3 (how the Lumia is held: Surface.ROTATION_*; apps with a fixed orientation
//                         still win, as on a phone). Touches stay in the stream's 768x1280 pixels: mapped here.
//                     'O' (send audio: older clients would not know 'U')
//                     'F' u32 seq: every frame up to seq received, sent as each one arrives (the pacer, RemotoStream)
//                     'Y' u32 length, PIN (typed into the Pixel's lock prompt + Enter) | 'Z' (cancel the lock prompt)
// Pixel -> client also 'k' u32 length, JSON {"on": bool}: the Pixel's own lock prompt (PIN) is up or gone (Watcher)
// Keyboard, clipboard, microphone (PLAN §68; RemotoKeys + ponte/app RemotoIme, RemotoMic):
//   server -> client  'I' u32 length, JSON {"on":bool,"type":text|number|pin|phone|email|url|password,
//                         "action":search|send|go|next|done|none,"multi":bool}: a field wants the keyboard / is gone
//                     'c' u32 length, UTF-8: copied on the Pixel | 'm' u8 1/0: an app records from the microphone
//   client -> server  'h' u16 the Lumia keyboard's height in display pixels (0 = down)
//                     'M' u32 length, u64 client_us, Opus packet (48 kHz mono, 20 ms): the Lumia's microphone
public final class Remoto {
    static int PORT = 8795;
    static String NAME = "remoto";   // args: [port [display name]] (a second instance for tests)
    static final int WIDTH = 768, HEIGHT = 1280, DPI = 320;   // Lumia 1020: 768x1280, 332 dpi

    private static VirtualDisplay display;
    private static int displayId;
    private static PowerManager.WakeLock awake;
    private static Input keys;
    private static Input keys0;   // the Pixel's own screen: its lock prompt

    public static void main(String[] args) throws Exception {
        Looper.prepareMainLooper();
        if (args.length > 0) PORT = Integer.parseInt(args[0]);
        if (args.length > 1) NAME = args[1];
        Sys.context();
        byte[] secret = Main.hex(new String(Files.readAllBytes(new File(Main.DIR, "key").toPath()),
                StandardCharsets.US_ASCII).trim());
        byte[] macKey = Main.hmac(secret, "mac".getBytes(StandardCharsets.US_ASCII));
        createDisplay();
        keys = new Input(displayId);
        keys0 = new Input(0);
        Main.log("remoto: display " + displayId + " " + WIDTH + "x" + HEIGHT + " " + DPI + " dpi, porta " + PORT);
        focusMainScreen();
        watchPulse();
        if (NAME.equals("remoto")) RemotoKeys.start(displayId);   // test instances keep the plain key events
        // Each client on its own thread: a new one (the Lumia back after a network change, or from the other path)
        // takes over at once. With one client at a time, a client gone without closing (4G lost, app killed: no FIN
        // through the tailnet) kept the session, and every later connection waited behind it forever.
        try (ServerSocket ss = new ServerSocket(PORT)) {
            while (true) {
                Socket s = ss.accept();
                new Thread(() -> {
                    try {
                        serve(s, macKey);
                    } catch (java.net.SocketTimeoutException t) {
                        Main.log("remoto: nessun segno dal client da 10 s, sessione chiusa");
                    } catch (Throwable t) {
                        Main.log("remoto: client " + t);
                    } finally {
                        try { s.close(); } catch (IOException ignored) {}
                    }
                }, "ponte-remoto-client").start();
            }
        }
    }

    private static final Object sessionLock = new Object();
    private static Session current;   // the client on the display

    private static void createDisplay() throws Exception {
        Constructor<DisplayManager> c = DisplayManager.class.getDeclaredConstructor(android.content.Context.class);
        c.setAccessible(true);
        DisplayManager dm = c.newInstance(Sys.context());
        int flags = VDisplay.PUBLIC | VDisplay.OWN_CONTENT_ONLY | VDisplay.SUPPORTS_TOUCH | VDisplay.DESTROY_CONTENT_ON_REMOVAL
                | VDisplay.TRUSTED | VDisplay.OWN_DISPLAY_GROUP | VDisplay.ALWAYS_UNLOCKED | VDisplay.TOUCH_FEEDBACK_DISABLED
                | VDisplay.OWN_FOCUS;
        // No STEAL_TOP_FOCUS_DISABLED (unlike VDisplay's default): touching here makes this the top focused display,
        // and screens the system opens by itself (Chrome's password manager, the unlock pattern to confirm the
        // device credential) open on the focused display: with the flag they went to the Pixel's own screen.
        display = dm.createVirtualDisplay(NAME, WIDTH, HEIGHT, DPI, null, flags);
        if (display == null) throw new IllegalStateException("createVirtualDisplay returned null");
        displayId = display.getDisplay().getDisplayId();
        VDisplay.hideIme(displayId);   // the Lumia's keyboard types here ('X'); no Pixel keyboard on the real screen
        // Rotation as on a phone: a virtual display is "fixed to user rotation" by default, so an app asking for
        // landscape (a video) got a small letterboxed picture. Not fixed, it rotates for such apps by itself; the
        // user rotation follows the Lumia ('W'). The surface stays 768x1280: the picture just turns in it.
        Main.sh("wm fixed-to-user-rotation -d " + displayId + " disabled; wm user-rotation -d " + displayId + " lock 0");
    }

    // Screen wake lock bound to the display (§63: its own power group dozes otherwise and its apps stop drawing).
    private static void wake(boolean on) {
        try {
            if (awake != null) { awake.release(); awake = null; }
            if (!on) return;
            PowerManager pm = Sys.context().getSystemService(PowerManager.class);
            awake = (PowerManager.WakeLock) PowerManager.class
                    .getMethod("newWakeLock", int.class, String.class, int.class)
                    .invoke(pm, PowerManager.SCREEN_DIM_WAKE_LOCK | PowerManager.ACQUIRE_CAUSES_WAKEUP, "ponte:remoto", displayId);
            awake.setReferenceCounted(false);
            awake.acquire();
        } catch (Exception e) {
            Main.log("remoto: wake " + e);
        }
    }

    // ---------------------------------------------------------------------------------------------- one session

    private static final class Session {
        final Socket socket;
        final OutputStream out;
        final boolean lan;   // the Lumia at home; else through tailscaled (127.0.0.1: the tailnet, often the relay)
        RemotoStream stream;
        volatile boolean alive = true;
        int frames;
        long bytes;
        final Pointers pointers = new Pointers();
        Thread audio;
        volatile RemotoMic mic;        // the Lumia's microphone as the Pixel's (PLAN §68)
        RemotoKeys.Client keysClient;  // the "Tastiera del Lumia" talks to this session

        // One writer thread, so no producer ever waits on the network. Control and audio go first; the video comes
        // from the pacer (RemotoStream), which never has more than the path takes in flight, so its queue stays
        // at a frame or two. Whatever is queued goes out in one write (fewer, fuller packets through the relay).
        static final int CTRL_QUEUE_BYTES = 32 << 10;   // ~4 s of Opus audio: older packets are useless
        final Object queueLock = new Object();
        final java.util.ArrayDeque<byte[]> videoQ = new java.util.ArrayDeque<>();
        final java.util.ArrayDeque<byte[]> ctrlQ = new java.util.ArrayDeque<>();
        int ctrlBytes;
        Thread writer;

        Session(Socket socket) throws IOException {
            this.socket = socket;
            this.out = socket.getOutputStream();
            this.lan = !socket.getInetAddress().isLoopbackAddress();
        }

        void enqueue(byte kind, byte[] head, byte[] body, int offset, int length) {
            int hn = head == null ? 0 : head.length;
            byte[] d = new byte[1 + hn + length];
            d[0] = kind;
            if (hn > 0) System.arraycopy(head, 0, d, 1, hn);
            if (length > 0) System.arraycopy(body, offset, d, 1 + hn, length);
            synchronized (queueLock) {
                if (!alive) return;
                // audio is the bulk of this queue: if the path stalls, old 20 ms packets are useless
                while (!ctrlQ.isEmpty() && ctrlBytes + d.length > CTRL_QUEUE_BYTES) ctrlBytes -= ctrlQ.poll().length;
                ctrlQ.add(d);
                ctrlBytes += d.length;
                queueLock.notifyAll();
            }
        }

        // RemotoStream.Sink: 'V' u32 length, u64 server_us, u32 seq, Annex-B access unit
        void video(byte[] frame, int length, int seq, boolean key, long captureUs) {
            byte[] d = new byte[17 + length];
            ByteBuffer h = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
            h.put((byte) 'V').putInt(length).putLong(captureUs).putInt(seq);
            System.arraycopy(frame, 0, d, 17, length);
            synchronized (queueLock) {
                if (!alive) return;
                videoQ.add(d);
                frames++;
                bytes += d.length;
                queueLock.notifyAll();
            }
        }

        void startWriter() {
            writer = new Thread(() -> {
                java.io.ByteArrayOutputStream batch = new java.io.ByteArrayOutputStream(1 << 17);
                try {
                    while (alive) {
                        batch.reset();
                        synchronized (queueLock) {
                            while (alive && ctrlQ.isEmpty() && videoQ.isEmpty()) queueLock.wait();
                            if (!alive) break;
                            while (!ctrlQ.isEmpty()) {
                                byte[] m = ctrlQ.poll();
                                ctrlBytes -= m.length;
                                batch.write(m, 0, m.length);
                            }
                            while (!videoQ.isEmpty() && batch.size() < (64 << 10)) {
                                byte[] m = videoQ.poll();
                                batch.write(m, 0, m.length);
                            }
                        }
                        batch.writeTo(out);
                        out.flush();
                    }
                } catch (Exception e) {
                    if (alive) Main.log("remoto: invio " + e);
                    alive = false;
                    try { socket.close(); } catch (IOException ignored) {}
                }
            }, "ponte-remoto-writer");
            writer.setPriority(Thread.MAX_PRIORITY);
            writer.start();
        }

        void kill() {   // from another thread: ends readLoop (its socket read fails) and so the session
            stopWriter();
        }

        void stopWriter() {
            synchronized (queueLock) {
                alive = false;
                queueLock.notifyAll();
            }
            try { socket.close(); } catch (IOException ignored) {}
            if (writer != null) try { writer.join(1000); } catch (InterruptedException ignored) {}
        }
    }

    private static void serve(Socket s, byte[] macKey) throws Exception {
        s.setTcpNoDelay(true);
        s.setSendBufferSize(64 << 10);   // the pacer bounds what is in flight; the kernel buffer only has to hold it
        Main.log("remoto: client " + s.getRemoteSocketAddress());
        Session session = new Session(s);
        session.startWriter();
        DataInputStream in = new DataInputStream(s.getInputStream());
        // auth: the ponte key, an HMAC-SHA256 of our nonce; a wrong answer closes at once
        byte[] nonce = new byte[16];
        new SecureRandom().nextBytes(nonce);
        session.enqueue((byte) 'C', null, nonce, 0, 16);
        s.setSoTimeout(10_000);
        // The client may send before answering (its info line 'S', pings 'p', the app list 'Q'): read them here,
        // the app list is sent once authenticated.
        boolean wantApps = false, wantAudio = false;
        int held = -1;
        for (int kind; (kind = in.readUnsignedByte()) != 'A'; ) {
            if (kind == 'S') Main.log("remoto: lumia: " + new String(readBlock(in), StandardCharsets.UTF_8));
            else if (kind == 'p') in.readFully(new byte[8]);
            else if (kind == 'Q') wantApps = true;
            else if (kind == 'O') wantAudio = true;
            else if (kind == 'W') held = in.readUnsignedByte() & 3;
            else { session.stopWriter(); return; }
        }
        byte[] answer = new byte[32];
        in.readFully(answer);
        if (!java.security.MessageDigest.isEqual(answer, Main.hmac(macKey, nonce))) {
            Main.log("remoto: chiave sbagliata da " + s.getRemoteSocketAddress());
            session.stopWriter();
            return;
        }
        // The client pings every 500 ms: 10 s of silence is a client that is gone (the timeout stays on).
        synchronized (sessionLock) {
            if (current != null) {
                Main.log("remoto: un nuovo client prende il posto del precedente");
                current.kill();
                long end = SystemClock.uptimeMillis() + 5000;
                while (current != null && SystemClock.uptimeMillis() < end) sessionLock.wait(500);
            }
            current = session;
        }
        if (held >= 0) rotate(held);
        session.stream = new RemotoStream(WIDTH, HEIGHT, session.lan, session::video);
        try {
            Surface surface = session.stream.start();
            synchronized (sessionLock) {
                if (current == session) {
                    wake(true);
                    display.setSurface(surface);
                }
            }
            if (wantApps) sendApps(session);
            if (wantAudio) startAudio(session);
            startWatcher(session);
            if (NAME.equals("remoto")) {
                session.keysClient = (k, body) -> session.enqueue((byte) k, le32(body.length), body, 0, body.length);
                RemotoKeys.sessionStart(session.keysClient);
                try {
                    session.mic = new RemotoMic((on, who) ->
                            session.enqueue((byte) 'm', null, new byte[] {(byte) (on ? 1 : 0)}, 0, 1));
                    session.mic.start();
                } catch (Exception e) {
                    Main.log("remoto: microfono non disponibile: " + e);
                    session.mic = null;
                }
                startCallYield(session);
            }
            readLoop(session, in);
        } finally {
            session.stopWriter();
            synchronized (sessionLock) {
                if (current == session) {   // a client that took over after a timeout owns the display already
                    focusMainScreen();
                    display.setSurface(null);
                    wake(false);
                }
            }
            session.pointers.clear();
            if (session.keysClient != null) RemotoKeys.sessionEnd(session.keysClient);
            RemotoMic mic = session.mic;
            session.mic = null;
            if (mic != null) mic.stop();
            session.stream.stop();
            if (NAME.equals("remoto")) showing(null);
            if (session.audio != null) try { session.audio.join(1000); } catch (InterruptedException ignored) {}
            Main.log("remoto: fine sessione, " + session.frames + " frame, " + session.bytes / 1024 + " KiB");
            synchronized (sessionLock) {
                if (current == session) current = null;
                sessionLock.notifyAll();
            }
        }
    }

    // ---------------------------------------------------------------------------------------------- audio

    // The Pixel's sound output (REMOTE_SUBMIX, as scrcpy's "output" source: the shell user holds
    // CAPTURE_AUDIO_OUTPUT; while it is captured the Pixel's speaker is silent) -> Opus 48 kHz stereo 20 ms frames,
    // 64 kbit/s -> 'U' u32 length, u64 server_us, Opus packet. Only while a client is on.
    static final int RATE = 48000, CHANNELS = 2, FRAME = 960;   // 20 ms

    private static void startAudio(Session session) {
        session.audio = new Thread(() -> {
            android.media.AudioRecord rec = null;
            MediaCodec enc = null;
            try {
                android.media.AudioFormat fmt = new android.media.AudioFormat.Builder()
                        .setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT).setSampleRate(RATE)
                        .setChannelMask(android.media.AudioFormat.CHANNEL_IN_STEREO).build();
                int min = android.media.AudioRecord.getMinBufferSize(RATE, android.media.AudioFormat.CHANNEL_IN_STEREO,
                        android.media.AudioFormat.ENCODING_PCM_16BIT);
                rec = new android.media.AudioRecord.Builder().setContext(Sys.context())
                        .setAudioSource(android.media.MediaRecorder.AudioSource.REMOTE_SUBMIX).setAudioFormat(fmt)
                        .setBufferSizeInBytes(Math.max(min, FRAME * 4 * 8)).build();
                MediaFormat f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, CHANNELS);
                // through the tailnet the path is often a relay and a mobile link: half the bits, still clear
                f.setInteger(MediaFormat.KEY_BIT_RATE, session.lan ? 64000 : 32000);
                enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
                enc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                enc.start();
                rec.startRecording();
                Main.log("remoto: audio " + enc.getName() + ", " + RATE + " Hz");
                byte[] pcm = new byte[FRAME * CHANNELS * 2];
                byte[] out = new byte[4096];
                ByteBuffer head = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN);
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                long frames = 0;
                while (session.alive) {
                    int got = 0;
                    while (got < pcm.length && session.alive) {
                        int r = rec.read(pcm, got, pcm.length - got);
                        if (r < 0) throw new IOException("AudioRecord.read " + r);
                        got += r;
                    }
                    int i = enc.dequeueInputBuffer(20_000);
                    if (i >= 0) {
                        ByteBuffer in = enc.getInputBuffer(i);
                        in.clear();
                        in.put(pcm, 0, got);
                        enc.queueInputBuffer(i, 0, got, frames * 1_000_000L / RATE, 0);
                        frames += got / (CHANNELS * 2);
                    }
                    for (int o; (o = enc.dequeueOutputBuffer(info, 0)) >= 0; ) {
                        ByteBuffer data = enc.getOutputBuffer(o);
                        int n = info.size;
                        boolean config = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                        if (!config && n > 0 && n <= out.length) {
                            data.position(info.offset);
                            data.get(out, 0, n);
                            head.clear();
                            head.putInt(n).putLong(SystemClock.elapsedRealtimeNanos() / 1000);
                            session.enqueue((byte) 'U', head.array(), out, 0, n);
                        }
                        enc.releaseOutputBuffer(o, false);
                    }
                }
            } catch (Exception e) {
                if (session.alive) Main.log("remoto: audio " + e);
            } finally {
                if (rec != null) try { rec.stop(); rec.release(); } catch (Exception ignored) {}
                if (enc != null) try { enc.stop(); enc.release(); } catch (Exception ignored) {}
            }
        }, "ponte-remoto-audio");
        session.audio.setPriority(Thread.MAX_PRIORITY);
        session.audio.start();
    }

    private static void readLoop(Session session, DataInputStream in) throws Exception {
        ByteBuffer le = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        byte[] tmp = new byte[16];
        while (session.alive) {
            int kind = in.read();
            if (kind < 0) return;
            try {
                if (!message(session, in, kind, le, tmp)) return;
            } catch (IOException e) {
                throw e;
            } catch (Exception e) {
                Main.log("remoto: messaggio " + (char) kind + ": " + e);   // a failed input never ends the session
            }
        }
    }

    private static boolean message(Session session, DataInputStream in, int kind, ByteBuffer le, byte[] tmp) throws Exception {
        if (kind == 'T' || kind == 'B' || kind == 'X' || kind == 'L' || kind == 'R' || kind == 'Y') lastInputMs = SystemClock.uptimeMillis();
        {
            switch (kind) {
                case 'p': {
                    in.readFully(tmp, 0, 8);
                    ByteBuffer r = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
                    r.putLong(SystemClock.elapsedRealtimeNanos() / 1000).put(tmp, 0, 8);
                    session.enqueue((byte) 'P', null, r.array(), 0, 16);
                    break;
                }
                case 'K': session.stream.keyFrame(); break;
                case 'O': if (session.audio == null) startAudio(session); break;   // the client plays audio
                case 'F': {   // the client has every frame up to this seq (sent as each one arrives)
                    in.readFully(tmp, 0, 4);
                    int seq = (tmp[0] & 0xFF) | (tmp[1] & 0xFF) << 8 | (tmp[2] & 0xFF) << 16 | (tmp[3] & 0xFF) << 24;
                    session.stream.onAck(seq, SystemClock.elapsedRealtimeNanos() / 1000);
                    break;
                }
                case 'S': Main.log("remoto: lumia: " + new String(readBlock(in), StandardCharsets.UTF_8)); break;
                case 'T': {
                    in.readFully(tmp, 0, 6);
                    le.clear();
                    le.put(tmp, 0, 6).flip();
                    int action = le.get() & 0xFF, id = le.get() & 0xFF;
                    int x = le.getShort() & 0xFFFF, y = le.getShort() & 0xFFFF;
                    session.pointers.touch(action, id, x, y);
                    break;
                }
                case 'B': {
                    in.readFully(tmp, 0, 2);
                    int code = (tmp[0] & 0xFF) | (tmp[1] & 0xFF) << 8;
                    if (!RemotoKeys.key(code)) keys.key(code);   // in a field: the keyboard's Enter does its action
                    break;
                }
                case 'h': {   // the Lumia's keyboard height in display pixels (0 = down): the keyboard strip follows
                    in.readFully(tmp, 0, 2);
                    RemotoKeys.height((tmp[0] & 0xFF) | (tmp[1] & 0xFF) << 8);
                    break;
                }
                case 'M': {   // the Lumia's microphone: u32 length, u64 client_us, Opus packet (48 kHz mono, 20 ms)
                    in.readFully(tmp, 0, 12);
                    int n = (tmp[0] & 0xFF) | (tmp[1] & 0xFF) << 8 | (tmp[2] & 0xFF) << 16 | (tmp[3] & 0xFF) << 24;
                    if (n < 0 || n > 4000) throw new IOException("pacchetto del microfono di " + n + " byte");
                    byte[] packet = new byte[n];
                    in.readFully(packet);
                    RemotoMic mic = session.mic;
                    if (mic != null) mic.packet(packet, n);
                    break;
                }
                case 'X': {
                    String text = new String(readBlock(in), StandardCharsets.UTF_8);
                    if (RemotoKeys.text(text)) break;   // a field is open: the keyboard commits it
                    // real keys where the keyboard map has them; runs of the others pasted (Input.paste)
                    StringBuilder other = new StringBuilder();
                    for (int i = 0; i < text.length(); ) {
                        int cp = text.codePointAt(i), n = Character.charCount(cp);
                        boolean typed = cp == '\n' || (n == 1 && keys.canType((char) cp));
                        if (typed && other.length() > 0) { keys.paste(other.toString()); other.setLength(0); }
                        if (cp == '\n') keys.key(android.view.KeyEvent.KEYCODE_ENTER);
                        else if (typed) keys.typeChar((char) cp);
                        else other.append(text, i, i + n);
                        i += n;
                    }
                    if (other.length() > 0) keys.paste(other.toString());
                    break;
                }
                case 'L': launch(new String(readBlock(in), StandardCharsets.UTF_8), false); break;
                case 'R': launch(new String(readBlock(in), StandardCharsets.UTF_8), true); break;
                case 'Q': sendApps(session); break;
                case 'q': sendOpen(session); break;
                case 'x': {
                    in.readFully(tmp, 0, 4);
                    int id = (tmp[0] & 0xFF) | (tmp[1] & 0xFF) << 8 | (tmp[2] & 0xFF) << 16 | (tmp[3] & 0xFF) << 24;
                    new Thread(() -> {
                        try {
                            close(id);
                            SystemClock.sleep(400);   // the removal is asynchronous: listed too early it is still there
                            sendOpen(session);
                        } catch (Exception e) {
                            Main.log("remoto: chiusura " + e);
                        }
                    }, "ponte-remoto-close").start();
                    break;
                }
                case 'W': rotate(in.readUnsignedByte() & 3); break;
                case 'Y': {   // the PIN for the lock prompt on the Pixel's screen: digits, then Enter (never logged)
                    String pin = new String(readBlock(in), StandardCharsets.UTF_8);
                    for (char ch : pin.toCharArray())
                        if (ch >= '0' && ch <= '9') keys0.key(android.view.KeyEvent.KEYCODE_0 + (ch - '0'));
                    keys0.key(android.view.KeyEvent.KEYCODE_ENTER);
                    Main.log("remoto: PIN inviato al riquadro di sblocco");
                    break;
                }
                case 'Z': keys0.key(android.view.KeyEvent.KEYCODE_BACK); break;
                default:
                    Main.log("remoto: tipo sconosciuto " + kind);
                    return false;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------- the real screen

    // What the system opens on the Pixel's own screen instead of ours, watched every 400 ms while a client is on:
    // - tasks an app starts there (Chrome -> Google's password manager in a task of its own) are moved here;
    // - the lock prompt (BiometricPrompt, Settings' ConfirmLock*) is a SECURE window that only exists on the
    //   default display: no capture can show it (black even in screencap). Its keys still work: the client is told
    //   ('k') and shows its own PIN box, the PIN comes back ('Y') and is typed there.
    // ---------------------------------------------------------------------------------------------- the agent's reads
    //
    // Accessibility (UiAutomation) learns a display's windows only while the display is on.
    // With no Lumia connected ours has no surface: when the agent (re)connects to accessibility, every one of its
    // reads waits 5 s for our windows, which never come, and its own screen reads as empty ("ponte bloccato", the
    // bridge broken since Remoto existed, 2026-11-07/08). Once the windows were seen they stay known, also after the
    // display goes dark again. So the agent asks (writes remoto.pulse: at its start and after a slow read) and the
    // display goes on for 0.6 s with a small throwaway surface.
    private static android.os.FileObserver pulseWatch;   // a field: a collected observer stops watching
    private static HandlerThread pulseThread;
    private static long lastPulse;

    private static void watchPulse() {
        pulseThread = new HandlerThread("ponte-remoto-pulse");
        pulseThread.start();
        pulseWatch = new android.os.FileObserver(new File(Main.DIR),
                android.os.FileObserver.CLOSE_WRITE | android.os.FileObserver.MOVED_TO) {
            @Override public void onEvent(int event, String path) {
                if ("remoto.pulse".equals(path)) pulse();
            }
        };
        pulseWatch.startWatching();
    }

    private static void pulse() {
        long now = SystemClock.uptimeMillis();
        if (now - lastPulse < 5000) return;
        lastPulse = now;
        android.media.ImageReader r = android.media.ImageReader.newInstance(WIDTH / 4, HEIGHT / 4,
                android.graphics.PixelFormat.RGBA_8888, 2);
        r.setOnImageAvailableListener(ir -> {
            android.media.Image i = ir.acquireLatestImage();
            if (i != null) i.close();
        }, new android.os.Handler(pulseThread.getLooper()));
        synchronized (sessionLock) {
            if (current != null) { r.close(); return; }   // a Lumia is on: the display is on already
            display.setSurface(r.getSurface());
            wake(true);
        }
        SystemClock.sleep(600);
        synchronized (sessionLock) {
            if (current == null) {
                focusMainScreen();
                display.setSurface(null);
                wake(false);
            }
        }
        r.close();
        Main.log("remoto: display acceso 0,6 s per l'accessibilità dell'agente");
    }

    // Our display has no STEAL_TOP_FOCUS_DISABLED (screens the system opens by itself must come here while a Lumia
    // uses it), so using it makes it the phone's top focused display, and it stayed so after the Lumia left. A dark
    // display holding the top focus froze accessibility's window lists (an app listed on both displays under an
    // old window id): a UiAutomation client saw an empty screen. When a session
    // ends (and after a pulse) the focus goes back to the Pixel's own screen, to the task already on top there:
    // nothing moves on it.
    static void focusMainScreen() {
        try {
            Object atm = Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null);
            java.util.List<?> tasks = (java.util.List<?>) atm.getClass().getMethod("getAllRootTaskInfosOnDisplay", int.class)
                    .invoke(atm, 0);
            for (Object t : tasks) {
                boolean visible = t.getClass().getField("visible").getBoolean(t);
                if (!visible) continue;
                int id = t.getClass().getField("taskId").getInt(t);
                atm.getClass().getMethod("setFocusedRootTask", int.class).invoke(atm, id);
                return;
            }
        } catch (Throwable e) {
            Main.log("remoto: fuoco allo schermo del Pixel: " + e);
        }
    }

    // What the Lumia is looking at, for other processes on the phone: the display is private, so the agent's
    // accessibility cannot see it. One package per line, rewritten every 400 ms while a client is on (the agent
    // takes a file older than 3 s as stale), deleted when the session ends.
    private static void showing(java.util.Set<String> packages) {
        File f = new File(Main.DIR, "remoto.top");
        if (packages == null) { f.delete(); return; }
        try {
            Files.write(f.toPath(), String.join("\n", packages).getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Main.log("remoto: remoto.top " + e);
        }
    }

    // Last input from the Lumia (touch, key, text, launch): a task that appears on the Pixel's own screen soon after
    // it was opened by what the Lumia did; one that appears with no recent input is the Pixel's user's, left alone
    // (the watcher used to move every new app away from someone holding the Pixel while a Lumia was connected).
    private static volatile long lastInputMs;

    // A call handled by another Ponte process owns the injected microphone: two injectors race for
    // the app's recorder and the first registered wins. That process writes call.active while a call rings or lasts;
    // the session's microphone stops meanwhile and comes back after.
    private static void startCallYield(Session session) {
        Thread t = new Thread(() -> {
            java.io.File flag = new java.io.File(Main.DIR, "call.active");
            boolean yielded = false;
            while (session.alive) {
                SystemClock.sleep(300);
                boolean call = flag.exists();
                RemotoMic m = session.mic;
                if (call && m != null) {
                    session.mic = null;
                    m.stop();
                    yielded = true;
                    session.enqueue((byte) 'm', null, new byte[] {0}, 0, 1);
                    Main.log("remoto: microfono ceduto alla chiamata");
                } else if (!call && yielded && m == null && session.alive) {
                    yielded = false;
                    try {
                        RemotoMic n = new RemotoMic((on, who) ->
                                session.enqueue((byte) 'm', null, new byte[] {(byte) (on ? 1 : 0)}, 0, 1));
                        n.start();
                        session.mic = n;
                        Main.log("remoto: microfono ripreso dopo la chiamata");
                    } catch (Exception e) {
                        Main.log("remoto: microfono non ripreso: " + e);
                    }
                }
            }
        }, "ponte-remoto-callyield");
        t.setDaemon(true);
        t.start();
    }

    private static void startWatcher(Session session) {
        if (!NAME.equals("remoto")) return;   // test instances (tools/remotonet) never touch the real screen
        Thread t = new Thread(() -> {
            java.util.Set<Integer> known = null;   // tasks already on the real screen when the client came
            boolean prompt = false;
            while (session.alive) {
                try {
                    boolean now = false;
                    java.util.Set<Integer> tasks = new java.util.HashSet<>();
                    java.util.Map<Integer, String> moves = new java.util.LinkedHashMap<>();
                    String title = null, type = null;
                    int disp = -1, task = -1;
                    java.util.Set<String> here = new java.util.TreeSet<>();   // apps visible on our display
                    for (String line : (Main.sh("dumpsys window visible") + "\n  Window #").split("\n")) {
                        String l = line.trim();
                        if (l.startsWith("Window #")) {
                            if (title != null && disp == displayId && "BASE_APPLICATION".equals(type) && title.indexOf('/') > 0)
                                here.add(title.substring(0, title.indexOf('/')));
                            if (title != null && disp == 0) {
                                if (title.contains("BiometricPrompt") || title.contains("ConfirmLock")
                                        || title.contains("ConfirmDeviceCredential")) now = true;
                                else if (task > 0 && "BASE_APPLICATION".equals(type)) {
                                    tasks.add(task);
                                    if (!title.contains("launcher") && !title.contains("Launcher")) moves.put(task, title);
                                }
                            }
                            int a = l.indexOf(" u0 "), b = l.lastIndexOf('}');
                            title = a > 0 && b > a ? l.substring(a + 4, b) : "";
                            type = null;
                            disp = -1;
                            task = -1;
                        } else if (l.startsWith("mDisplayId=")) {
                            String[] f = l.split("[ =]");
                            disp = Integer.parseInt(f[1]);
                            for (int i = 0; i + 1 < f.length; i++) if (f[i].equals("taskId")) task = Integer.parseInt(f[i + 1]);
                        } else if (l.startsWith("mAttrs=") && l.contains(" ty=")) {
                            String r = l.substring(l.indexOf(" ty=") + 4);
                            type = r.split(" ")[0];
                        }
                    }
                    showing(here);
                    if (known == null) known = tasks;
                    for (java.util.Map.Entry<Integer, String> m : moves.entrySet()) {
                        if (known.contains(m.getKey())) continue;
                        if (SystemClock.uptimeMillis() - lastInputMs > 5000) {   // not ours: the Pixel's user opened it
                            known.add(m.getKey());
                            continue;
                        }
                        Main.sh("am display move-stack " + m.getKey() + " " + displayId);
                        Main.log("remoto: spostato qui dallo schermo del Pixel: " + m.getValue());
                    }
                    if (now != prompt) {
                        prompt = now;
                        Main.log("remoto: riquadro di sblocco " + (now ? "aperto" : "chiuso"));
                        byte[] j = new JSONObject().put("on", now).toString().getBytes(StandardCharsets.UTF_8);
                        ByteBuffer h = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(j.length);
                        session.enqueue((byte) 'k', h.array(), j, 0, j.length);
                    }
                } catch (Exception e) {
                    if (session.alive) Main.log("remoto: watcher " + e);
                }
                SystemClock.sleep(400);
            }
        }, "ponte-remoto-watch");
        t.setDaemon(true);
        t.start();
    }

    private static void sendApps(Session session) throws Exception {
        byte[] list = apps().toString().getBytes(StandardCharsets.UTF_8);
        ByteBuffer h = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(list.length);
        session.enqueue((byte) 'a', h.array(), list, 0, list.length);
    }

    // ---------------------------------------------------------------------------------------------- open apps

    // Root tasks on our display, front first ("am stack list" lists them in z-order).
    private static JSONArray openTasks() throws Exception {
        java.util.Map<String, String> labels = new java.util.HashMap<>();
        for (Object o : iterable(apps())) labels.put(((JSONObject) o).getString("p"), ((JSONObject) o).getString("l"));
        JSONArray list = new JSONArray();
        int root = -1;
        boolean ours = false, named = false;
        for (String line : Main.sh("am stack list").split("\n")) {
            String l = line.trim();
            if (l.startsWith("RootTask id=")) {
                root = Integer.parseInt(l.substring(12, l.indexOf(' ', 12)));
                ours = l.contains(" displayId=" + displayId + " ");
                named = false;
            } else if (ours && !named && l.startsWith("taskId=")) {
                int colon = l.indexOf(": "), slash = l.indexOf('/');
                if (colon < 0 || slash < colon) continue;
                String pkg = l.substring(colon + 2, slash);
                named = true;
                list.put(new JSONObject().put("t", root).put("p", pkg).put("l", labels.getOrDefault(pkg, pkg)));
            }
        }
        return list;
    }

    private static void sendOpen(Session session) throws Exception {
        byte[] list = openTasks().toString().getBytes(StandardCharsets.UTF_8);
        ByteBuffer h = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(list.length);
        session.enqueue((byte) 'o', h.array(), list, 0, list.length);
    }

    // Closes a root task of our display as a swipe in Recents would (the app's process may stay cached); 0 = all.
    private static void close(int root) throws Exception {
        StringBuilder cmd = new StringBuilder();
        JSONArray open = openTasks();
        for (int i = 0; i < open.length(); i++) {
            int t = open.getJSONObject(i).getInt("t");
            if (root == 0 || t == root) cmd.append("am stack remove ").append(t).append("; ");
        }
        if (cmd.length() == 0) return;
        Main.sh(cmd.toString());
        Main.log("remoto: chiuse " + (root == 0 ? "tutte le app (" + open.length() + ")" : "il task " + root));
    }

    // ---------------------------------------------------------------------------------------------- rotation

    private static volatile int wantRotation = -1;
    private static Thread rotator;

    // The user rotation of our display ('W'): "wm" is a process of its own (~100 ms), so it runs off the input
    // thread, the latest wish wins.
    private static synchronized void rotate(int r) {
        wantRotation = r;
        if (rotator != null) return;
        rotator = new Thread(() -> {
            int done = -1;
            while (true) {
                int r2;
                synchronized (Remoto.class) {
                    r2 = wantRotation;
                    if (r2 == done) { rotator = null; return; }
                }
                try {
                    Main.sh("wm user-rotation -d " + displayId + " lock " + r2);
                } catch (Exception e) {
                    Main.log("remoto: rotazione " + e);
                }
                done = r2;
                Main.log("remoto: Lumia ruotato " + r2 * 90 + " gradi, schermo ora " + rotation() * 90);
            }
        }, "ponte-remoto-rotate");
        rotator.start();
    }

    // The display's current rotation (also when an app chose it).
    static int rotation() {
        try {
            return display.getDisplay().getRotation();
        } catch (Exception e) {
            return 0;
        }
    }

    private static byte[] le32(int n) {
        return new byte[] {(byte) n, (byte) (n >> 8), (byte) (n >> 16), (byte) (n >> 24)};
    }

    private static byte[] readBlock(DataInputStream in) throws IOException {
        byte[] n = new byte[4];
        in.readFully(n);
        int len = (n[0] & 0xFF) | (n[1] & 0xFF) << 8 | (n[2] & 0xFF) << 16 | (n[3] & 0xFF) << 24;
        if (len < 0 || len > 1 << 20) throw new IOException("blocco di " + len + " byte");
        byte[] b = new byte[len];
        in.readFully(b);
        return b;
    }

    // ---------------------------------------------------------------------------------------------- apps

    // Launchable activities of user 0. PackageManager queries from this shell process come back empty (the system
    // context has no user of its own), so the list comes from "cmd package"; labels from PackageManager when it can.
    private static JSONArray apps() throws Exception {
        PackageManager pm = Sys.context().getPackageManager();
        String out = Main.sh("cmd package query-activities --brief --user 0 -a android.intent.action.MAIN"
                + " -c android.intent.category.LAUNCHER");
        List<JSONObject> list = new ArrayList<>();
        java.util.Set<String> seen = new java.util.HashSet<>();
        for (String line : out.split("\n")) {
            line = line.trim();
            int slash = line.indexOf('/');
            if (slash <= 0 || line.contains(" ") || !seen.add(line)) continue;
            String pkg = line.substring(0, slash), cls = line.substring(slash + 1);
            if (cls.startsWith(".")) cls = pkg + cls;
            String label = pkg;
            try {
                label = pm.getActivityInfo(new android.content.ComponentName(pkg, cls), 0).loadLabel(pm).toString();
            } catch (Exception ignored) {}
            JSONObject o = new JSONObject();
            o.put("p", pkg);
            o.put("c", cls);
            o.put("l", label);
            list.add(o);
        }
        list.sort((x, y) -> x.optString("l").compareToIgnoreCase(y.optString("l")));
        return new JSONArray(list);
    }

    // Starts the app's launcher activity on the display; restart = force-stop first (a fresh start, also the way
    // out of an app that hangs). Picture-in-picture is turned off for it: a mini player (Netflix) left on this
    // display has no way to be closed from the Lumia.
    private static void launch(String pkg, boolean restart) throws Exception {
        for (Object o : iterable(apps())) {
            JSONObject a = (JSONObject) o;
            if (!a.getString("p").equals(pkg)) continue;
            String out = Main.sh("appops set " + pkg + " PICTURE_IN_PICTURE ignore; "
                    + (restart ? "am force-stop " + pkg + "; " : "")
                    + "am start --display " + displayId + " -f 0x10200000 -n " + a.getString("p") + "/" + a.getString("c"));
            Main.log("remoto: " + (restart ? "riavvio " : "avvio ") + pkg + ": " + out.trim());
            return;
        }
        Main.log("remoto: app sconosciuta " + pkg);
    }

    private static Iterable<Object> iterable(JSONArray a) {
        List<Object> l = new ArrayList<>();
        for (int i = 0; i < a.length(); i++) l.add(a.opt(i));
        return l;
    }

    // ---------------------------------------------------------------------------------------------- touch

    // Pointers down on the display; every event carries all of them, as a touchscreen's do.
    private static final class Pointers {
        private static final int INJECT_ASYNC = 0;
        private final int[] ids = new int[10];
        private final float[] xs = new float[10], ys = new float[10];
        private int count;
        private long downTime;
        private Object im;
        private Method inject, setDisplayId;

        void clear() {
            if (count > 0) emit(MotionEvent.ACTION_CANCEL, 0);
            count = 0;
        }

        void touch(int action, int id, int sx, int sy) throws Exception {
            // stream pixels (the surface, always 768x1280) -> the display's own rotated coordinates
            int x = sx, y = sy;
            switch (rotation()) {
                case 1: x = sy; y = WIDTH - 1 - sx; break;
                case 2: x = WIDTH - 1 - sx; y = HEIGHT - 1 - sy; break;
                case 3: x = HEIGHT - 1 - sy; y = sx; break;
            }
            int at = -1;
            for (int i = 0; i < count; i++) if (ids[i] == id) at = i;
            long now = SystemClock.uptimeMillis();
            switch (action) {
                case 0:   // down
                    if (at >= 0 || count == ids.length) return;
                    if (count == 0) downTime = now;
                    ids[count] = id; xs[count] = x; ys[count] = y;
                    count++;
                    emit(count == 1 ? MotionEvent.ACTION_DOWN
                            : MotionEvent.ACTION_POINTER_DOWN | (count - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT, now);
                    break;
                case 2:   // move
                    if (at < 0) return;
                    xs[at] = x; ys[at] = y;
                    emit(MotionEvent.ACTION_MOVE, now);
                    break;
                case 1:   // up
                    if (at < 0) return;
                    xs[at] = x; ys[at] = y;
                    emit(count == 1 ? MotionEvent.ACTION_UP
                            : MotionEvent.ACTION_POINTER_UP | at << MotionEvent.ACTION_POINTER_INDEX_SHIFT, now);
                    for (int i = at; i < count - 1; i++) { ids[i] = ids[i + 1]; xs[i] = xs[i + 1]; ys[i] = ys[i + 1]; }
                    count--;
                    break;
                default:   // cancel
                    clear();
            }
        }

        private void emit(int action, long now) {
            try {
                if (im == null) {
                    Class<?> cls;
                    try {
                        cls = Class.forName("android.hardware.input.InputManagerGlobal");
                    } catch (ClassNotFoundException e) {
                        cls = InputManager.class;
                    }
                    Method gi = cls.getDeclaredMethod("getInstance");
                    gi.setAccessible(true);
                    im = gi.invoke(null);
                    inject = cls.getMethod("injectInputEvent", InputEvent.class, int.class);
                    setDisplayId = InputEvent.class.getMethod("setDisplayId", int.class);
                }
                if (now == 0) now = SystemClock.uptimeMillis();
                MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[count];
                MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[count];
                for (int i = 0; i < count; i++) {
                    props[i] = new MotionEvent.PointerProperties();
                    props[i].id = ids[i];
                    props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
                    coords[i] = new MotionEvent.PointerCoords();
                    coords[i].x = xs[i];
                    coords[i].y = ys[i];
                    coords[i].pressure = 1;
                    coords[i].size = 0.05f;
                }
                MotionEvent e = MotionEvent.obtain(downTime, now, action, count, props, coords, 0, 0, 1, 1, 0, 0,
                        InputDevice.SOURCE_TOUCHSCREEN, 0);
                setDisplayId.invoke(e, displayId);
                inject.invoke(im, e, INJECT_ASYNC);
                e.recycle();
            } catch (Exception ex) {
                Main.log("remoto: tocco " + ex);
            }
        }
    }

    private Remoto() {}
}
