package dumbphone.ponte;

import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

// Remoto's video for one client (PLAN §65): the frames go out only as fast as the path takes them.
//
//   virtual display -> SurfaceTexture (GL, always latched at once: the display never waits) -> drawn into the
//   encoder's input surface only when the pacer says so -> H.264 -> the session's writer -> the Lumia, which
//   acknowledges every frame ('F' seq) as soon as it has it.
//
// Nothing ever queues behind a slow path: while the frames in flight fill the window, new screen content just
// replaces the texture, and the next frame encodes the latest screen as a normal P-frame (no chain is broken, so
// no key frame is needed to recover). The old design encoded every display frame and dropped them when the path
// fell behind; a dropped P-frame broke the chain, the key frame it asked for was big, and through the relay that
// was a loop of resets (measured: frames 1.8 s old, 10 resets in 40 s at 1 Mbit/s).
//
// Rate: delivery-rate samples from the acks (bytes acknowledged / time, as BBR) give the path's capacity; the
// window of bytes in flight is twice capacity x minimum round trip, the encoder's bitrate follows the queueing
// delay (up while it stays low and the bitrate is used, down to 85% of what arrives when it grows).
// Still screens: after a change, the same picture is encoded again a few times, so a page that came blurry
// through a slow path sharpens in place (the display itself sends nothing while the screen is still).
final class RemotoStream {
    interface Sink {   // the session's writer
        void video(byte[] frame, int length, int seq, boolean key, long captureUs);
    }

    static final int MAX_BITRATE = 6_000_000, MIN_BITRATE = 150_000;
    private static final long MIN_INTERVAL_US = 16_000;    // 60 fps at most
    private static final long REFINE_AFTER_US = 120_000;   // still this long: sharpen
    private static final int REFINES = 4;

    private final int width, height;
    private final Sink sink;
    private final boolean lan;
    private MediaCodec codec;
    private Surface input;
    private HandlerThread glThread;
    private Handler gl;
    private EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
    private EGLContext ctx = EGL14.EGL_NO_CONTEXT;
    private EGLSurface encSurface = EGL14.EGL_NO_SURFACE;
    private SurfaceTexture texture;
    private Surface displaySurface;
    private int program, texId, aPos, aTex, uTexM;
    private final float[] texMatrix = new float[16];
    private FloatBuffer quad;
    private Thread drainer;
    private volatile boolean alive = true;
    private byte[] config;

    // ---- pacing state (guarded by this)
    private boolean dirty;              // the display has content not encoded yet
    private boolean haveImage;          // the texture holds a picture
    private int refines;                // refinement frames left for the current still picture
    private long lastSubmitUs, lastContentUs;
    private long transitionStart;       // first display frame after the screen was still
    private boolean transitionSent;     // ...and it went out (instant feedback); the rest of a short animation waits
    private int probe;                  // round-trip probe: 1 = let the pipe empty, 2 = one frame (probeSeq) measures it
    private int probeSeq;
    private boolean encoding;           // a frame is in the encoder
    private long encodeStartUs;
    private boolean wantKey;
    private int seq;
    // frames in flight: seq -> size, send time, delivered-at-send (rate samples)
    private final int[] fSize = new int[1024];
    private final long[] fSent = new long[1024], fDelivered = new long[1024], fDeliveredAt = new long[1024],
            fFirstSent = new long[1024];
    private final boolean[] fAppLimited = new boolean[1024];
    private final boolean[] fAlone = new boolean[1024];   // sent on an empty pipe: its round trip has no queue in it
    private double avgFrame = 8_000;                       // bytes, recent frames
    private int acked = -1;
    private long inflight;
    private long delivered, deliveredAt, firstSentAt;
    private long appLimitedUntil;       // samples of frames sent before 'delivered' reaches this are app-limited
    // estimates
    private long minRttUs = Long.MAX_VALUE, minRttAt;
    private double srttUs, queueUs;
    private int lastAckedSize;
    private long lastCut, lastSample;
    private long rateBps;               // capacity estimate (windowed max of rate samples)
    private final long[] rateSamples = new long[16], rateSampleAt = new long[16];
    private int rateIdx;
    private int bitrate;
    private long lastBitrateChange, sentBytesWindow, sentWindowStart;
    // stats
    private int frames, refineFrames, skipped, keyFrames;
    private long bytes, statsAt;

    RemotoStream(int width, int height, boolean lan, Sink sink) {
        this.width = width;
        this.height = height;
        this.lan = lan;
        this.sink = sink;
        bitrate = lan ? 4_000_000 : 1_000_000;
    }

    // ------------------------------------------------------------------------------------------ setup

    Surface start() throws Exception {
        MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, 60);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 3600);   // key frames only on request ('K', new client)
        f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline);
        f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel41);
        f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
        f.setInteger(MediaFormat.KEY_LATENCY, 1);
        f.setInteger(MediaFormat.KEY_PRIORITY, 0);
        f.setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1);
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        try {
            codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        } catch (Exception e) {
            Main.log("remoto: configure " + e + ", provo baseline");
            codec.release();
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
            codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        }
        input = codec.createInputSurface();
        codec.start();
        Main.log("remoto: encoder " + codec.getName() + ", " + (lan ? "LAN" : "tailnet") + ", " + bitrate / 1000 + " kbit/s iniziali");
        drainer = new Thread(this::drain, "ponte-remoto-enc");
        drainer.setPriority(Thread.MAX_PRIORITY);
        drainer.start();
        glThread = new HandlerThread("ponte-remoto-gl", android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
        glThread.start();
        gl = new Handler(glThread.getLooper());
        Surface[] out = new Surface[1];
        Exception[] err = new Exception[1];
        Object done = new Object();
        gl.post(() -> {
            try {
                setupGl();
                out[0] = displaySurface;
            } catch (Exception e) {
                err[0] = e;
            }
            synchronized (done) { done.notifyAll(); }
        });
        synchronized (done) {
            while (out[0] == null && err[0] == null) done.wait(2000);
        }
        if (err[0] != null) throw err[0];
        statsAt = now();
        return out[0];
    }

    private void setupGl() {
        dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] v = new int[2];
        if (!EGL14.eglInitialize(dpy, v, 0, v, 1)) throw new IllegalStateException("eglInitialize");
        int[] attribs = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, 0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] n = new int[1];
        if (!EGL14.eglChooseConfig(dpy, attribs, 0, configs, 0, 1, n, 0) || n[0] < 1)
            throw new IllegalStateException("eglChooseConfig");
        ctx = EGL14.eglCreateContext(dpy, configs[0], EGL14.EGL_NO_CONTEXT, new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        encSurface = EGL14.eglCreateWindowSurface(dpy, configs[0], input, new int[] {EGL14.EGL_NONE}, 0);
        if (!EGL14.eglMakeCurrent(dpy, encSurface, encSurface, ctx)) throw new IllegalStateException("eglMakeCurrent");
        program = program(
                "attribute vec4 aPos; attribute vec4 aTex; uniform mat4 uTexM; varying vec2 vTex;\n"
                        + "void main() { gl_Position = aPos; vTex = (uTexM * aTex).xy; }",
                "#extension GL_OES_EGL_image_external : require\nprecision mediump float; varying vec2 vTex;\n"
                        + "uniform samplerExternalOES sTex; void main() { gl_FragColor = texture2D(sTex, vTex); }");
        aPos = GLES20.glGetAttribLocation(program, "aPos");
        aTex = GLES20.glGetAttribLocation(program, "aTex");
        uTexM = GLES20.glGetUniformLocation(program, "uTexM");
        float[] q = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
        quad = ByteBuffer.allocateDirect(q.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        quad.put(q).position(0);
        int[] t = new int[1];
        GLES20.glGenTextures(1, t, 0);
        texId = t[0];
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        texture = new SurfaceTexture(texId);
        texture.setDefaultBufferSize(width, height);
        texture.setOnFrameAvailableListener(st -> onDisplayFrame(), gl);
        displaySurface = new Surface(texture);
    }

    private static int program(String vs, String fs) {
        int p = GLES20.glCreateProgram();
        GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs));
        GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs));
        GLES20.glLinkProgram(p);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) throw new IllegalStateException("link: " + GLES20.glGetProgramInfoLog(p));
        return p;
    }

    private static int shader(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) throw new IllegalStateException("shader: " + GLES20.glGetShaderInfoLog(s));
        return s;
    }

    void stop() {
        alive = false;
        if (gl != null) {
            Object done = new Object();
            boolean[] fin = new boolean[1];
            gl.post(() -> {
                try {
                    if (texture != null) texture.release();
                    if (displaySurface != null) displaySurface.release();
                    if (dpy != EGL14.EGL_NO_DISPLAY) {
                        EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                        if (encSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, encSurface);
                        if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
                        EGL14.eglReleaseThread();
                    }
                } catch (Exception ignored) {
                }
                synchronized (done) { fin[0] = true; done.notifyAll(); }
            });
            synchronized (done) {
                long end = SystemClock.uptimeMillis() + 2000;
                while (!fin[0] && SystemClock.uptimeMillis() < end) try { done.wait(200); } catch (InterruptedException e) { break; }
            }
            glThread.quitSafely();
        }
        try { codec.signalEndOfInputStream(); } catch (Exception ignored) {}
        try { if (drainer != null) drainer.join(500); } catch (InterruptedException ignored) {}
        try { codec.stop(); } catch (Exception ignored) {}
        try { codec.release(); } catch (Exception ignored) {}
        if (input != null) input.release();
    }

    // ------------------------------------------------------------------------------------------ the pacer

    private static long now() { return SystemClock.elapsedRealtimeNanos() / 1000; }

    // GL thread: the display composed a new picture. Latched at once (the display must never wait for us), encoded
    // when the pacer allows.
    private void onDisplayFrame() {
        if (!alive) return;
        try {
            texture.updateTexImage();
        } catch (Exception e) {
            Main.log("remoto: updateTexImage " + e);
            return;
        }
        synchronized (this) {
            long t = now();
            if (haveImage && dirty) skipped++;   // replaced before it was sent: the path is slower than the screen
            if (t - lastContentUs > 150_000) { transitionStart = t; transitionSent = false; }
            haveImage = true;
            dirty = true;
            lastContentUs = t;
            refines = REFINES;
        }
        pump();
    }

    // GL thread: send what the pacer allows now, and come back when it may allow more.
    private void pump() {
        if (!alive) return;
        boolean refine;
        long wait;
        synchronized (this) {
            long t = now();
            if (encoding && t - encodeStartUs > 200_000) encoding = false;   // never lose the pacer on a silent encoder
            if (!haveImage || encoding) return;   // the drainer calls back when the frame is out
            boolean want = dirty || wantKey || (refines > 0 && t - lastContentUs >= REFINE_AFTER_US);
            if (!want) {
                if (refines > 0) gl.postDelayed(this::pump, (REFINE_AFTER_US - (t - lastContentUs)) / 1000 + 1);
                return;
            }
            if (inflight > 0 && (inflight >= window() || probe == 1)) return;   // an ack calls back
            // Slow path: the first frame of a change goes at once (the tap is seen), the rest of a short animation
            // (a page turn, a menu opening) waits until it ends, up to 300 ms: its middle frames are big and stale
            // by the time they arrive (52 KB of half-slid page = 0.4 s at 1 Mbit/s). Long animations (games,
            // scrolling) flow normally after the 300 ms.
            if (dirty && !wantKey && slow() && transitionSent && t - transitionStart < 300_000 && t - lastContentUs < 50_000) {
                gl.postDelayed(this::pump, 20);
                return;
            }
            wait = MIN_INTERVAL_US - (t - lastSubmitUs);
            if (wait > 0) {
                gl.postDelayed(this::pump, wait / 1000 + 1);
                return;
            }
            refine = !dirty && !wantKey;
            if (refine) refines--;
            if (dirty) transitionSent = true;
            if (probe == 1) { probe = 2; probeSeq = seq; }   // the pipe is empty: this frame measures the bare path
            dirty = false;
            if (wantKey) {
                wantKey = false;
                Bundle b = new Bundle();
                b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
                codec.setParameters(b);
            }
            encoding = true;
            encodeStartUs = t;
            lastSubmitUs = t;
            if (refine) refineFrames++;
        }
        try {
            GLES20.glViewport(0, 0, width, height);
            GLES20.glUseProgram(program);
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId);
            texture.getTransformMatrix(texMatrix);
            GLES20.glUniformMatrix4fv(uTexM, 1, false, texMatrix, 0);
            quad.position(0);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad);
            GLES20.glEnableVertexAttribArray(aPos);
            quad.position(2);
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quad);
            GLES20.glEnableVertexAttribArray(aTex);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
            EGLExt.eglPresentationTimeANDROID(dpy, encSurface, SystemClock.elapsedRealtimeNanos());
            EGL14.eglSwapBuffers(dpy, encSurface);
        } catch (Exception e) {
            Main.log("remoto: disegno " + e);
            synchronized (this) { encoding = false; }
        }
    }

    // Bytes allowed in flight: 2 x capacity x bare round trip (BBR's cwnd gain: at most about one round trip of
    // queue, and enough room that the delivery samples can still see the whole capacity; 1.5 capped itself at 65%
    // of a 2 Mbit/s link), at least a frame and a half. Never from the measured average round trip: with the pipe
    // always full it measures its own queue and grows without end (a 0.5 s queue on the game scene before).
    private long window() {
        long cap = rateBps > 0 ? rateBps : bitrate;
        long rtt = minRttUs == Long.MAX_VALUE ? (lan ? 20_000 : 150_000) : minRttUs;
        return Math.max(cap / 4 * rtt / 1_000_000, Math.max(lan ? 128 << 10 : 16 << 10, (long) (1.5 * avgFrame)));
    }

    private boolean slow() { return !lan && (rateBps == 0 || rateBps < 3_000_000); }

    void keyFrame() {
        synchronized (this) {
            wantKey = true;
        }
        if (gl != null) gl.post(this::pump);
    }

    // ------------------------------------------------------------------------------------------ encoder output

    private void drain() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        byte[] buf = new byte[1 << 16];
        try {
            while (alive) {
                int i = codec.dequeueOutputBuffer(info, 100_000);
                if (i < 0) continue;
                ByteBuffer data = codec.getOutputBuffer(i);
                int n = info.size;
                if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    config = new byte[n];
                    data.position(info.offset);
                    data.get(config);
                    codec.releaseOutputBuffer(i, false);
                    continue;
                }
                boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                data.position(info.offset);
                boolean hasSps = n > 4 && (data.get(info.offset + 4) & 0x1F) == 7;
                int extra = key && !hasSps && config != null ? config.length : 0;   // the Lumia wants SPS first
                if (buf.length < n + extra) buf = new byte[n + extra + (1 << 16)];
                if (extra > 0) System.arraycopy(config, 0, buf, 0, extra);
                data.get(buf, extra, n);
                codec.releaseOutputBuffer(i, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
                if (n == 0) continue;
                int s;
                long t = now();
                int size = n + extra + 17;
                synchronized (this) {
                    s = seq++;
                    int k = s % 1024;
                    fSize[k] = size;
                    fSent[k] = t;
                    if (inflight == 0) { firstSentAt = t; deliveredAt = t; }   // BBR: an empty pipe restarts the interval
                    fDelivered[k] = delivered;
                    fDeliveredAt[k] = deliveredAt;
                    fFirstSent[k] = firstSentAt;
                    // app-limited: this frame went out with room to spare (nothing was waiting on the window)
                    fAppLimited[k] = inflight + size < window() / 2;
                    fAlone[k] = inflight == 0;
                    avgFrame = avgFrame * 0.9 + size * 0.1;
                    inflight += size;
                    encoding = false;
                    frames++;
                    bytes += size;
                    sentBytesWindow += size;
                    if (key || extra > 0 || hasSps) keyFrames++;
                }
                sink.video(buf, n + extra, s, key || extra > 0 || hasSps, t);
                if (gl != null) gl.post(this::pump);
            }
        } catch (Exception e) {
            if (alive) Main.log("remoto: encoder " + e);
        }
    }

    // ------------------------------------------------------------------------------------------ acks and rate

    // Reader thread: the Lumia has every frame up to s.
    void onAck(int s, long t) {
        boolean more;
        synchronized (this) {
            if (s <= acked || s >= seq) return;
            lastAckedSize = fSize[s % 1024];
            for (int q = Math.max(acked + 1, s - 1023); q <= s; q++) {
                int k = q % 1024;
                delivered += fSize[k];
                inflight -= fSize[k];
                fSize[k] = 0;
            }
            if (inflight < 0) inflight = 0;
            acked = s;
            int k = s % 1024;
            long rtt = t - fSent[k];
            deliveredAt = t;
            firstSentAt = fSent[k];
            // round trip: the minimum over 10 s is the path without queues. The queueing delay is what is left after
            // the frame's own transmission time (a big frame on a slow path is slow, not queued).
            if (rtt > 0) {
                // A frame that went alone took: the bare round trip + its own transmission (size / capacity). Only
                // those give the bare round trip; never more than half of the sample is taken off (the capacity
                // estimate may be low: at the start it is unknown, and a 52 KB key frame at the floor rate made it
                // "1 ms").
                long cap = Math.max(rateBps, bitrate);
                long tx = Math.min(rtt / 2, (long) (lastAckedSize * 8_000_000.0 / cap));
                long bare = rtt - tx;
                boolean probeDone = probe == 2 && s >= probeSeq;
                if (fAlone[k] && (bare < minRttUs || probeDone)) { minRttUs = bare; minRttAt = t; }
                if (probeDone) probe = 0;
                long q = Math.max(0, bare - minRttUs);
                queueUs = queueUs * 0.8 + q * 0.2;
                srttUs = srttUs == 0 ? rtt : srttUs * 0.85 + rtt * 0.15;
                // The bare round trip is only seen with an empty pipe; a game keeps it full. Every 20 s: let it drain,
                // send one frame alone and take its round trip as the new minimum (BBR's ProbeRTT, about 0.3 s of
                // fewer frames).
                if (t - minRttAt > 20_000_000 && probe == 0) probe = 1;
                // A queue of more than a bare round trip that lasts: the capacity estimate is stale (the link got
                // slower; the max filter would hold the old value for 2.5 s): take what arrives now.
                if (queueUs > Math.max(minRttUs, 80_000) && t - lastCut > 500_000) {
                    lastCut = t;
                    long recent = lastSample > 0 ? lastSample : rateBps;
                    rateBps = Math.max(MIN_BITRATE, Math.min(rateBps * 8 / 10, recent));
                    for (int i = 0; i < rateSamples.length; i++) rateSamples[i] = Math.min(rateSamples[i], rateBps);
                }
            }
            // delivery rate (BBR): bytes delivered since this frame was sent, over the longer of the send and the
            // ack intervals (an ack burst must not look like a fast path)
            long ackElapsed = t - fDeliveredAt[k], sendElapsed = fSent[k] - fFirstSent[k];
            long interval = Math.max(ackElapsed, sendElapsed);
            if (interval > 2_000) {
                long sample = (delivered - fDelivered[k]) * 8_000_000 / interval;
                if (!fAppLimited[k]) lastSample = sample;
                if (!fAppLimited[k] || sample > rateBps) addRate(sample, t);
            }
            adapt(t);
            more = inflight < window();
            stats(t);
        }
        if (more && gl != null) gl.post(this::pump);
    }

    private long fSizeAtAck(int s) { return lastAckedSize; }

    private void addRate(long sample, long t) {
        rateSamples[rateIdx] = sample;
        rateSampleAt[rateIdx] = t;
        rateIdx = (rateIdx + 1) % rateSamples.length;
        long max = 0;
        for (int i = 0; i < rateSamples.length; i++)
            if (t - rateSampleAt[i] < 2_500_000) max = Math.max(max, rateSamples[i]);
        rateBps = max;
    }

    // The encoder's bitrate follows the path's capacity (80%: audio, headers, and room for a change of scene).
    // The queue is the pacer's business (the window): the bitrate only sets how good each frame is, and the
    // frame rate falls by itself when frames are bigger than the path carries.
    private void adapt(long t) {
        if (rateBps <= 0 || t - lastBitrateChange < 300_000) return;
        int next = (int) Math.max(MIN_BITRATE, Math.min(MAX_BITRATE, rateBps * 8 / 10));
        if (Math.abs(next - bitrate) > bitrate / 10) {
            bitrate = next;
            lastBitrateChange = t;
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate);
            try { codec.setParameters(b); } catch (Exception ignored) {}
        }
    }

    private void stats(long t) {
        if (t - statsAt < 10_000_000) return;
        double secs = (t - statsAt) / 1e6;
        Main.log(String.format(java.util.Locale.ROOT,
                "remoto: %.1f fps (%d nitidezza, %d saltati, %d chiave), %.0f kbit/s, bitrate %d, capacita %d kbit/s, "
                        + "rtt min %d / medio %d ms, coda %d ms, in volo %d KB / finestra %d KB",
                frames / secs, refineFrames, skipped, keyFrames, bytes * 8 / secs / 1000, bitrate / 1000, rateBps / 1000,
                minRttUs / 1000, (long) srttUs / 1000, (long) queueUs / 1000, inflight / 1024, window() / 1024));
        frames = refineFrames = skipped = keyFrames = 0;
        bytes = 0;
        statsAt = t;
    }
}
