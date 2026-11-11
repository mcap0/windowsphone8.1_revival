package dumbphone.pixelfoto;

import android.accessibilityservice.AccessibilityService;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;

import java.util.List;

/**
 * The Pixel's screen is broken: when SystemUI asks "Allow USB debugging?" this ticks "Always allow from this
 * computer" and presses OK, so adb survives reboots and replugs. Only for this PC's key: the dialog must show
 * its fingerprint. Enabled once from adb:
 * settings put secure enabled_accessibility_services dumbphone.pixelfoto/.AdbGuard
 */
public final class AdbGuard extends AccessibilityService {
    // MD5 fingerprint of this PC's ~/.android/adbkey.pub (build.sh -> assets/adbfp): any other computer is left alone
    private String allowed;

    @Override
    protected void onServiceConnected() {
        try (java.io.InputStream in = getAssets().open("adbfp")) {
            allowed = new String(Exif.readAll(in), java.nio.charset.StandardCharsets.US_ASCII).trim();
        } catch (java.io.IOException e) {
            allowed = null;
        }
        Log.i("AdbGuard attivo per " + allowed);
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent e) {
        if (e.getPackageName() == null || !"com.android.systemui".contentEquals(e.getPackageName())) return;
        AccessibilityNodeInfo root = getRootInActiveWindow();
        if (root == null) return;
        try {
            // com.android.systemui.usb.UsbDebuggingActivity: an alert with the RSA fingerprint, a CheckBox
            // (android:id/alwaysUse on Android 10) and button1 = OK. Only that dialog: it must mention USB debugging.
            if (find(root, "debug USB") == null && find(root, "USB debugging") == null) return;
            if (allowed == null || allowed.length() < 47 || find(root, allowed) == null) {
                Log.i("AdbGuard: richiesta di debug USB da un altro computer, lasciata stare");
                return;
            }
            AccessibilityNodeInfo box = checkbox(root);
            List<AccessibilityNodeInfo> ok = root.findAccessibilityNodeInfosByViewId("android:id/button1");
            if (box == null || ok.isEmpty()) return;
            if (!box.isChecked()) box.performAction(AccessibilityNodeInfo.ACTION_CLICK);
            ok.get(0).performAction(AccessibilityNodeInfo.ACTION_CLICK);
            Log.i("debug USB consentito (sempre) dalla richiesta di SystemUI");
        } catch (RuntimeException ex) {
            Log.i("AdbGuard: " + ex);
        }
    }

    private static AccessibilityNodeInfo find(AccessibilityNodeInfo root, String text) {
        List<AccessibilityNodeInfo> l = root.findAccessibilityNodeInfosByText(text);
        return l.isEmpty() ? null : l.get(0);
    }

    private static AccessibilityNodeInfo checkbox(AccessibilityNodeInfo n) {
        if (n == null) return null;
        if (n.getClassName() != null && "android.widget.CheckBox".contentEquals(n.getClassName())) return n;
        for (int i = 0; i < n.getChildCount(); i++) {
            AccessibilityNodeInfo c = checkbox(n.getChild(i));
            if (c != null) return c;
        }
        return null;
    }

    @Override
    public void onInterrupt() { }
}
