package dumbphone.pixelfoto;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Starts the receiver at boot (and after an update of the app). */
public final class Boot extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        c.startForegroundService(new Intent(c, Receiver.class));
    }
}
