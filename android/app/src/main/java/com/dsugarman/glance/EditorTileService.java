package com.dsugarman.glance;

import android.app.PendingIntent;
import android.content.Intent;
import android.os.Build;
import android.service.quicksettings.TileService;

/**
 * Quick Settings tile that opens the audio editor. Add it from
 * Quick Settings → Edit tiles → drag "Glance Editor" into the active grid.
 * Works from the lock screen on most Android versions.
 */
public class EditorTileService extends TileService {
    @Override
    public void onClick() {
        super.onClick();
        Intent i = new Intent(this, EditorActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        // Android 14+ requires PendingIntent for startActivityAndCollapse.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            PendingIntent pi = PendingIntent.getActivity(this, 0, i,
                    PendingIntent.FLAG_IMMUTABLE);
            startActivityAndCollapse(pi);
        } else {
            // Pre-14: legacy API took the Intent directly.
            startActivityAndCollapse(i);
        }
    }
}
