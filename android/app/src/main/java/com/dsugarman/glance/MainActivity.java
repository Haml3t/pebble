package com.dsugarman.glance;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import com.getpebble.android.kit.PebbleKit;

public class MainActivity extends Activity {

    private static final int REQ_RECORD_AUDIO = 1;

    private TextView statusPerm;
    private TextView statusMic;
    private TextView statusWatch;
    private TextView lastSent;
    private Button micButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusPerm = findViewById(R.id.status_perm);
        statusMic = findViewById(R.id.status_mic);
        statusWatch = findViewById(R.id.status_watch);
        lastSent = findViewById(R.id.last_sent);

        Button grant = findViewById(R.id.grant_button);
        grant.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS));
            }
        });

        micButton = findViewById(R.id.mic_button);
        micButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    requestPermissions(
                            new String[]{Manifest.permission.RECORD_AUDIO},
                            REQ_RECORD_AUDIO);
                }
            }
        });

        Button editorButton = findViewById(R.id.editor_button);
        editorButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(MainActivity.this, EditorActivity.class));
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        boolean granted = isListenerEnabled();
        statusPerm.setText(granted ? R.string.status_granted : R.string.status_denied);

        boolean micGranted = checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
        statusMic.setText(micGranted ? R.string.status_mic_granted : R.string.status_mic_denied);
        micButton.setVisibility(micGranted ? View.GONE : View.VISIBLE);

        boolean connected = PebbleKit.isWatchConnected(this);
        statusWatch.setText("Pebble: " + (connected ? "CONNECTED" : "not connected"));

        int todaySecs = MetronomeService.peekTodaySeconds(this);
        int todayMin = (todaySecs <= 0) ? 0 : (todaySecs + 59) / 60;
        boolean recording = MetronomeService.isCurrentlyRecording(this);
        String last = MediaListenerService.getLastSentSummary();
        String recLine = recording
                ? "🔴 RECORDING — open Metronome on watch is being captured"
                : "⚪ not recording";
        lastSent.setText(recLine
                + "\nmetronome today: " + todayMin + " min (" + todaySecs + " s ticked)"
                + "\nnow playing → " + (last == null ? "(none)" : last));
    }

    private boolean isListenerEnabled() {
        String flat = Settings.Secure.getString(getContentResolver(),
                "enabled_notification_listeners");
        if (flat == null) return false;
        ComponentName cn = new ComponentName(this, MediaListenerService.class);
        return flat.contains(cn.flattenToString());
    }
}
