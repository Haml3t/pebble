package com.dsugarman.glance;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

/**
 * Invisible trampoline activity that exists solely to put the app in TOP
 * process state for a fraction of a second so {@link MetronomeService} can
 * be started with FOREGROUND_SERVICE_TYPE_MICROPHONE.
 *
 * Why this contortion is necessary on Android 14+: launching a microphone-
 * type foreground service requires the app to be in a "while-in-use" state.
 * A {@link android.content.BroadcastReceiver}-initiated start is considered
 * background-FGS-state (BFGS), which is *not* while-in-use, and the platform
 * throws SecurityException ("the app must be in the eligible state /
 * exemptions to access the foreground only permission") right inside
 * {@code Service.startForeground(...)}. Having an Activity in onCreate
 * briefly puts the process in TOP, which IS while-in-use.
 *
 * SYSTEM_ALERT_WINDOW is the exemption that lets us start this Activity
 * from a background BroadcastReceiver in the first place. The activity
 * declares no UI and finishes synchronously, so visually nothing happens
 * on the phone.
 */
public class MetronomeTriggerActivity extends Activity {
    private static final String TAG = "GlanceMetTrampoline";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Suppress the open/close animation so even a fast eye doesn't see
        // a flash. The Translucent.NoTitleBar theme suppresses any window
        // rendering; this just kills the cross-fade.
        overridePendingTransition(0, 0);

        Intent in = getIntent();
        Intent fwd = new Intent(this, MetronomeService.class);
        if (in != null && in.getExtras() != null) {
            fwd.putExtras(in.getExtras());
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(fwd);
            } else {
                startService(fwd);
            }
        } catch (Throwable t) {
            Log.e(TAG, "trampoline → service start failed", t);
        }
        finish();
        overridePendingTransition(0, 0);
    }
}
