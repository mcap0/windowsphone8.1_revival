package dumbphone.ponte;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.PowerManager;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Constructor;
import java.nio.ByteBuffer;

// An invisible screen for an app driven from outside. Created as the shell user, so it can be trusted, keep its own focus
// and stay usable while the phone is locked. Frames land in an ImageReader: that is our screenshot.
final class VDisplay {
    // DisplayManager.VIRTUAL_DISPLAY_FLAG_* (several are hidden)
    static final int PUBLIC = 1, OWN_CONTENT_ONLY = 1 << 3, SUPPORTS_TOUCH = 1 << 6,
            DESTROY_CONTENT_ON_REMOVAL = 1 << 8, TRUSTED = 1 << 10, OWN_DISPLAY_GROUP = 1 << 11,
            ALWAYS_UNLOCKED = 1 << 12, TOUCH_FEEDBACK_DISABLED = 1 << 13, OWN_FOCUS = 1 << 14,
            // Android 14+: starting an app or injecting keys here must not make this the top focused display,
            // or the real screen loses focus (its keyboard and keys went to the virtual display, taps were "stolen")
            STEAL_TOP_FOCUS_DISABLED = 1 << 16;

    final int width, height, dpi;
    private final ImageReader reader;
    private final VirtualDisplay display;

    VDisplay(int width, int height, int dpi, Handler handler) throws Exception {
        this.width = width;
        this.height = height;
        this.dpi = dpi;
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
        // keep only the newest frame, so the display never stalls waiting for us
        reader.setOnImageAvailableListener(r -> {
            Image img = r.acquireLatestImage();
            if (img == null) return;
            synchronized (this) {
                if (latest != null) latest.close();
                latest = img;
            }
        }, handler);
        Constructor<DisplayManager> c = DisplayManager.class.getDeclaredConstructor(android.content.Context.class);
        c.setAccessible(true);
        DisplayManager dm = c.newInstance(Sys.context());
        int flags = PUBLIC | OWN_CONTENT_ONLY | SUPPORTS_TOUCH | DESTROY_CONTENT_ON_REMOVAL | TRUSTED
                | OWN_DISPLAY_GROUP | ALWAYS_UNLOCKED | TOUCH_FEEDBACK_DISABLED | OWN_FOCUS | STEAL_TOP_FOCUS_DISABLED;
        display = dm.createVirtualDisplay("ponte", width, height, dpi, reader.getSurface(), flags);
        if (display == null) throw new IllegalStateException("createVirtualDisplay returned null");
        hideIme(id());
        wake();
    }

    // OWN_DISPLAY_GROUP gives this display its own power group, with its own idle timeout, and no touch ever reaches
    // it. When it times out the group dozes: the app stays alive but its window is stopped and accessibility lists
    // no window ("screen NONE"), and neither "am start" nor key events wake it. A screen wake lock bound to this
    // display (newWakeLock with a display id, hidden) wakes that group alone and keeps it awake. Called again when
    // the screen is found empty, in case something put the group to sleep anyway.
    synchronized void wake() {
        try {
            if (awake != null) awake.release();
            PowerManager pm = Sys.context().getSystemService(PowerManager.class);
            awake = (PowerManager.WakeLock) PowerManager.class
                    .getMethod("newWakeLock", int.class, String.class, int.class)
                    .invoke(pm, PowerManager.SCREEN_DIM_WAKE_LOCK | PowerManager.ACQUIRE_CAUSES_WAKEUP, "ponte:display", id());
            awake.setReferenceCounted(false);
            awake.acquire();
        } catch (Exception e) {
            Main.log("wake: " + e);
        }
    }
    private PowerManager.WakeLock awake;

    // By default an on-screen keyboard asked for on a virtual display shows up on the real screen: the user's.
    // DISPLAY_IME_POLICY_HIDE keeps it away; the agent types with key events, it never needs one.
    static void hideIme(int displayId) throws Exception {
        Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
        Object wms = wmg.getMethod("getWindowManagerService").invoke(null);
        wms.getClass().getMethod("setDisplayImePolicy", int.class, int.class).invoke(wms, displayId, 2);
    }

    int id() { return display.getDisplay().getDisplayId(); }

    // Is this display (another process's, e.g. Remoto's) showing anything? A virtual display with no surface is OFF.
    static boolean isOn(int displayId) {
        try {
            synchronized (VDisplay.class) {
                if (dm == null) {
                    Constructor<DisplayManager> c = DisplayManager.class.getDeclaredConstructor(android.content.Context.class);
                    c.setAccessible(true);
                    dm = c.newInstance(Sys.context());
                }
            }
            android.view.Display disp = dm.getDisplay(displayId);
            return disp != null && disp.getState() == android.view.Display.STATE_ON;
        } catch (Exception e) {
            return true;   // unknown: hands off, as for the real screen
        }
    }
    private static DisplayManager dm;

    // Latest frame as a bitmap (copy), or null if nothing was drawn yet (or the frame went away meanwhile).
    synchronized Bitmap frame() {
        Image img = latest;
        if (img == null) return null;
        try {
            Image.Plane p = img.getPlanes()[0];
            ByteBuffer buf = p.getBuffer();
            buf.rewind();   // a previous copy left it at the end: "Buffer not large enough for pixels"
            int stride = p.getRowStride() / p.getPixelStride();
            int need = p.getRowStride() * height;
            if (buf.remaining() < need) {
                // the last row may come without its padding
                ByteBuffer full = ByteBuffer.allocate(need);
                full.put(buf);
                full.rewind();
                buf = full;
            }
            Bitmap bmp = Bitmap.createBitmap(stride, height, Bitmap.Config.ARGB_8888);
            bmp.copyPixelsFromBuffer(buf);
            return stride == width ? bmp : Bitmap.createBitmap(bmp, 0, 0, width, height);
        } catch (RuntimeException e) {
            Main.log("frame: " + e);
            return null;
        }
    }

    // Latest frame as PNG, or null if nothing was drawn yet.
    synchronized byte[] screenshotPng() {
        Bitmap bmp = frame();
        if (bmp == null) return null;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        bmp.compress(Bitmap.CompressFormat.PNG, 100, out);
        return out.toByteArray();
    }
    private Image latest;

    void release() {
        synchronized (this) { if (awake != null) awake.release(); }
        display.release();
        reader.close();
    }
}
