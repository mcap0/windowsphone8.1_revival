package dumbphone.ponte;

import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

// The agent runs through app_process as the shell user (like scrcpy), with no Application. Sys builds the
// minimum the framework classes need: an ActivityThread and a Context that says it is com.android.shell.
final class Sys {
    static final String PKG = "com.android.shell";
    static final int SHELL_UID = 2000;
    private static Context context;

    static synchronized Context context() {
        if (context == null) {
            try {
                Class<?> at = Class.forName("android.app.ActivityThread");
                Constructor<?> c = at.getDeclaredConstructor();
                c.setAccessible(true);
                Object thread = c.newInstance();
                Field cur = at.getDeclaredField("sCurrentActivityThread");
                cur.setAccessible(true);
                cur.set(null, thread);
                Field sys = at.getDeclaredField("mSystemThread");
                sys.setAccessible(true);
                sys.setBoolean(thread, true);
                Method gsc = at.getDeclaredMethod("getSystemContext");
                Context base = (Context) gsc.invoke(thread);
                context = new ShellContext(base);
            } catch (Exception e) {
                throw new RuntimeException("cannot build a context", e);
            }
        }
        return context;
    }

    static final class ShellContext extends ContextWrapper {
        ShellContext(Context base) { super(base); }
        @Override public String getPackageName() { return PKG; }
        @Override public String getOpPackageName() { return PKG; }
        @Override public AttributionSource getAttributionSource() {
            return new AttributionSource.Builder(SHELL_UID).setPackageName(PKG).build();
        }
        @Override public Context getApplicationContext() { return this; }
    }

    private Sys() {}
}
