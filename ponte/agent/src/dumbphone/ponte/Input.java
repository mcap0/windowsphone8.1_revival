package dumbphone.ponte;

import android.hardware.input.InputManager;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;

import java.lang.reflect.Method;

// Real key events on the virtual display, as a hardware keyboard would send them (the "RDP" keyboard).
final class Input {
    // InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_RESULT: wait until the event is dispatched, not until the app
    // has finished handling it (WAIT_FOR_FINISH could hang the bridge worker).
    private static final int INJECT_WAIT_FOR_RESULT = 1;
    private final Object im;
    private final Method inject, setDisplayId;
    private final KeyCharacterMap map = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
    private final int displayId;

    Input(int displayId) throws Exception {
        this.displayId = displayId;
        // Android 14+: InputManagerGlobal; older: InputManager.getInstance()
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

    private boolean send(KeyEvent e) throws Exception {
        KeyEvent k = KeyEvent.changeTimeRepeat(e, SystemClock.uptimeMillis(), 0);
        k.setSource(InputDevice.SOURCE_KEYBOARD);
        setDisplayId.invoke(k, displayId);
        return (Boolean) inject.invoke(im, k, INJECT_WAIT_FOR_RESULT);
    }

    boolean key(int keyCode) throws Exception {
        long t = SystemClock.uptimeMillis();
        return send(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, keyCode, 0))
                && send(new KeyEvent(t, t, KeyEvent.ACTION_UP, keyCode, 0));
    }

    // Characters with no key (accents, emoji): through the clipboard and the Paste key, which every text field takes
    // (Views, Compose, WebView). The shell user may set the clipboard as com.android.shell.
    boolean paste(String text) throws Exception {
        if (clipboard == null) {
            Object binder = Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "clipboard");
            clipboard = Class.forName("android.content.IClipboard$Stub").getMethod("asInterface", android.os.IBinder.class)
                    .invoke(null, binder);
            for (Method m : clipboard.getClass().getMethods()) if (m.getName().equals("setPrimaryClip")) setClip = m;
        }
        // setPrimaryClip(ClipData, String callingPackage, [String attributionTag,] int userId, [int deviceId])
        Class<?>[] types = setClip.getParameterTypes();
        Object[] args = new Object[types.length];
        args[0] = android.content.ClipData.newPlainText("remoto", text);
        args[1] = Sys.PKG;
        for (int i = 2; i < types.length; i++) args[i] = types[i] == int.class ? (Object) 0 : null;
        setClip.invoke(clipboard, args);
        return key(KeyEvent.KEYCODE_PASTE);
    }
    private Object clipboard;
    private Method setClip;

    boolean canType(char c) { return map.getEvents(new char[] {c}) != null; }

    // Types one character with real key events; false if the keyboard map has no key for it (emoji, most accents).
    boolean typeChar(char c) throws Exception {
        KeyEvent[] evs = map.getEvents(new char[] {c});
        if (evs == null) return false;
        for (KeyEvent e : evs) if (!send(e)) return false;
        return true;
    }
}
