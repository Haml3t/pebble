package com.dsugarman.glance;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import com.getpebble.android.kit.PebbleKit;

public class MainActivity extends Activity {

    private TextView statusPerm;
    private TextView statusWatch;
    private TextView lastSent;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusPerm = findViewById(R.id.status_perm);
        statusWatch = findViewById(R.id.status_watch);
        lastSent = findViewById(R.id.last_sent);

        Button grant = findViewById(R.id.grant_button);
        grant.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS));
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        boolean granted = isListenerEnabled();
        statusPerm.setText(granted ? R.string.status_granted : R.string.status_denied);

        boolean connected = PebbleKit.isWatchConnected(this);
        statusWatch.setText("Pebble: " + (connected ? "CONNECTED" : "not connected"));

        String last = MediaListenerService.getLastSentSummary();
        lastSent.setText(last == null ? "(no track sent yet)" : "last → " + last);
    }

    private boolean isListenerEnabled() {
        String flat = Settings.Secure.getString(getContentResolver(),
                "enabled_notification_listeners");
        if (flat == null) return false;
        ComponentName cn = new ComponentName(this, MediaListenerService.class);
        return flat.contains(cn.flattenToString());
    }
}
