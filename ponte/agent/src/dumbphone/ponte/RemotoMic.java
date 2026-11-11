package dumbphone.ponte;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecordingConfiguration;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.os.Handler;
import android.os.HandlerThread;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.List;

// The Lumia's microphone as the Pixel's (PLAN §68), without root: an AudioPolicy whose mix takes every recorder that
// asks for the microphone (MIC, VOICE_RECOGNITION, VOICE_COMMUNICATION, CAMCORDER, UNPROCESSED, VOICE_PERFORMANCE)
// and feeds it from our AudioTrack instead of the real microphone (loop-back injection, as Android Auto does; the
// shell user holds MODIFY_AUDIO_ROUTING). Registered only while a Lumia is on Remoto. When an app starts recording
// (AudioRecordingCallback) the Lumia is told ('m' 1) and sends its microphone as Opus ('M'); 'm' 0 when no app
// records any more. Measured 2026-11-08 with a test recorder: our 1 kHz tone came in instead of the room.
final class RemotoMic {
    static final int RATE = 48000, FRAME = 960;   // 20 ms mono
    private static final int[] SOURCES = { MediaRecorder.AudioSource.MIC, MediaRecorder.AudioSource.VOICE_RECOGNITION,
            MediaRecorder.AudioSource.VOICE_COMMUNICATION, MediaRecorder.AudioSource.CAMCORDER,
            MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_PERFORMANCE };

    interface Wanted { void wanted(boolean on, String who); }

    private final Wanted wanted;
    private AudioManager am;
    private Object policy;
    private AudioTrack track;
    private HandlerThread thread;
    private AudioManager.AudioRecordingCallback callback;
    private volatile boolean alive;
    private boolean recording;
    private MediaCodec decoder;
    private Thread feeder;
    private final ArrayDeque<short[]> pcm = new ArrayDeque<>();
    private boolean primed;
    int packets, underruns, dropped;

    RemotoMic(Wanted wanted) { this.wanted = wanted; }

    void start() throws Exception {
        Context ctx = Sys.context();
        am = ctx.getSystemService(AudioManager.class);
        AudioFormat fmt = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(RATE)
                .setChannelMask(AudioFormat.CHANNEL_IN_MONO).build();
        Class<?> ruleB = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
        Object rb = ruleB.getConstructor().newInstance();
        Method setPreset = AudioAttributes.Builder.class.getMethod("setCapturePreset", int.class);
        Method addRule = ruleB.getMethod("addMixRule", int.class, Object.class);
        int byPreset = Class.forName("android.media.audiopolicy.AudioMixingRule").getField("RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET").getInt(null);
        for (int src : SOURCES) {
            AudioAttributes.Builder ab = new AudioAttributes.Builder();
            setPreset.invoke(ab, src);
            addRule.invoke(rb, byPreset, ab.build());
        }
        ruleB.getMethod("setTargetMixRole", int.class).invoke(rb, 1);   // MIX_ROLE_INJECTOR
        Object rule = ruleB.getMethod("build").invoke(rb);
        Class<?> mixC = Class.forName("android.media.audiopolicy.AudioMix");
        Class<?> mixB = Class.forName("android.media.audiopolicy.AudioMix$Builder");
        Object mb = mixB.getConstructor(rule.getClass()).newInstance(rule);
        mixB.getMethod("setFormat", AudioFormat.class).invoke(mb, fmt);
        mixB.getMethod("setRouteFlags", int.class).invoke(mb, mixC.getField("ROUTE_FLAG_LOOP_BACK").getInt(null));
        Object mix = mixB.getMethod("build").invoke(mb);
        Class<?> polB = Class.forName("android.media.audiopolicy.AudioPolicy$Builder");
        Object pb = polB.getConstructor(Context.class).newInstance(ctx);
        polB.getMethod("addMix", mixC).invoke(pb, mix);
        policy = polB.getMethod("build").invoke(pb);
        int r = (int) AudioManager.class.getMethod("registerAudioPolicy", policy.getClass()).invoke(am, policy);
        if (r != 0) throw new IllegalStateException("registerAudioPolicy " + r);
        track = (AudioTrack) policy.getClass().getMethod("createAudioTrackSource", mixC).invoke(policy, mix);
        track.play();
        alive = true;
        feeder = new Thread(this::feed, "ponte-remoto-mic");
        feeder.setPriority(Thread.MAX_PRIORITY);
        feeder.start();
        thread = new HandlerThread("ponte-remoto-mic-cb");
        thread.start();
        callback = new AudioManager.AudioRecordingCallback() {
            @Override public void onRecordingConfigChanged(List<AudioRecordingConfiguration> configs) { check(configs); }
        };
        am.registerAudioRecordingCallback(callback, new Handler(thread.getLooper()));
        check(am.getActiveRecordingConfigurations());
    }

    // Someone other than us (the shell: Remoto's own sound capture) records from the microphone.
    private synchronized void check(List<AudioRecordingConfiguration> configs) {
        String who = null;
        for (AudioRecordingConfiguration c : configs) {
            int src = c.getClientAudioSource(), uid = -1;
            try { uid = (int) AudioRecordingConfiguration.class.getMethod("getClientUid").invoke(c); } catch (Exception ignored) {}
            if (uid == Sys.SHELL_UID) continue;
            for (int s : SOURCES) if (s == src) {
                String pkg = "?";
                try { pkg = (String) AudioRecordingConfiguration.class.getMethod("getClientPackageName").invoke(c); } catch (Exception ignored) {}
                who = pkg;
            }
        }
        boolean on = who != null;
        if (on == recording) return;
        recording = on;
        Main.log("remoto: microfono " + (on ? "chiesto da " + who : "non più usato") + " (pacchetti " + packets
                + ", vuoti " + underruns + ", scartati " + dropped + ")");
        wanted.wanted(on, who);
    }

    // ---------------------------------------------------------------------------------------- the Lumia's sound

    /** One Opus packet from the Lumia (48 kHz mono, 20 ms), on the session's reader thread. */
    void packet(byte[] data, int n) {
        if (!alive) return;
        try {
            if (decoder == null) decoder = opusDecoder();
            int in = decoder.dequeueInputBuffer(20_000);
            if (in >= 0) {
                ByteBuffer b = decoder.getInputBuffer(in);
                b.clear();
                b.put(data, 0, n);
                decoder.queueInputBuffer(in, 0, n, packets * 20_000L, 0);
                packets++;
            }
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            for (int out; (out = decoder.dequeueOutputBuffer(info, 0)) != MediaCodec.INFO_TRY_AGAIN_LATER; ) {
                if (out < 0) continue;   // format changed
                ByteBuffer o = decoder.getOutputBuffer(out);
                o.order(ByteOrder.LITTLE_ENDIAN);
                short[] s = new short[info.size / 2];
                o.position(info.offset);
                o.asShortBuffer().get(s);
                decoder.releaseOutputBuffer(out, false);
                synchronized (pcm) {
                    pcm.add(s);
                    // more than 300 ms waiting: the oldest is useless (a burst after a stall)
                    while (pcm.size() > 15) { pcm.poll(); dropped++; }
                    pcm.notifyAll();
                }
            }
        } catch (Exception e) {
            Main.log("remoto: microfono: decoder " + e);
            if (decoder != null) try { decoder.release(); } catch (Exception ignored) {}
            decoder = null;
        }
    }

    private static MediaCodec opusDecoder() throws Exception {
        MediaFormat f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, 1);
        ByteBuffer head = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN);
        head.put("OpusHead".getBytes(java.nio.charset.StandardCharsets.US_ASCII)).put((byte) 1).put((byte) 1)
                .putShort((short) 0).putInt(RATE).putShort((short) 0).put((byte) 0).flip();
        f.setByteBuffer("csd-0", head);
        f.setByteBuffer("csd-1", ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(0, 0));
        f.setByteBuffer("csd-2", ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(0, 80_000_000L));
        MediaCodec d = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
        d.configure(f, null, null, 0);
        d.start();
        return d;
    }

    // The injected microphone runs at its own pace: the Lumia's sound when there is some, silence in between. After
    // a gap it waits for 60 ms of sound so that network jitter does not chop every word.
    private void feed() {
        short[] silence = new short[FRAME];
        while (alive) {
            short[] next = null;
            synchronized (pcm) {
                if (!primed && pcm.size() >= 3) primed = true;
                if (primed) {
                    next = pcm.poll();
                    if (next == null) { primed = false; underruns++; }
                }
            }
            short[] b = next != null ? next : silence;
            int w = track.write(b, 0, b.length);   // blocks at the recorder's pace
            if (w < 0) { Main.log("remoto: microfono: write " + w); android.os.SystemClock.sleep(20); }
        }
    }

    void stop() {
        alive = false;
        try {
            if (feeder != null) feeder.join(500);   // its write returns within 20 ms
        } catch (InterruptedException ignored) {}
        try {
            if (callback != null) am.unregisterAudioRecordingCallback(callback);
            if (thread != null) thread.quitSafely();
            if (policy != null) AudioManager.class.getMethod("unregisterAudioPolicy", policy.getClass()).invoke(am, policy);
            if (track != null) track.release();
            if (decoder != null) decoder.release();
        } catch (Exception e) {
            Main.log("remoto: microfono: stop " + e);
        }
        Main.log("remoto: microfono: fine (pacchetti " + packets + ", vuoti " + underruns + ", scartati " + dropped + ")");
    }
}
