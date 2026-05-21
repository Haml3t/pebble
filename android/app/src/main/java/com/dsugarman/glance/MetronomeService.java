package com.dsugarman.glance;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import com.getpebble.android.kit.PebbleKit;
import com.getpebble.android.kit.util.PebbleDictionary;
import java.io.File;
import java.io.FileWriter;
import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;
import java.util.UUID;

/**
 * Foreground service backing the Pebble metronome watchapp.
 *
 *  - Records phone-mic audio for the entire time the watch metronome app is
 *    open (including idle stretches where the metronome isn't ticking) — per
 *    the user spec, the recording is "the practice", not just "the ticks".
 *  - Tracks BPM range during the open session for the file name.
 *  - Accumulates per-day ticking time in SharedPreferences. Today/week are
 *    derived on demand for the watchapp's stats line and for the Glance
 *    watchface chip.
 *
 * Started by {@link MediaListenerService} when it receives a metronome
 * AppMessage from the watch. Forwarded events arrive as Intent extras keyed
 * by {@link #EXTRA_EVENT}.
 */
public class MetronomeService extends Service {

    private static final String TAG = "GlanceMet";

    // Metronome watchapp UUID. Keep in sync with metronome/package.json.
    static final UUID METRONOME_UUID =
            UUID.fromString("b7c974f6-3542-4d62-882e-faa24bc64906");

    // Glance watchface UUID — used so we can push the daily-minutes chip
    // unprompted when the metronome reports a session.
    static final UUID GLANCE_UUID =
            UUID.fromString("5b5b6a8e-1f5f-4f6e-9a1f-3b9f1a2c4d5e");

    // AppMessage keys for the metronome app. Auto-assigned by the Pebble
    // build from metronome/package.json messageKeys order, starting at
    // 10000. Update both ends together if you reorder.
    static final int KEY_METRONOME_OPENED      = 10000;
    static final int KEY_METRONOME_CLOSED      = 10001;
    static final int KEY_TICK_STARTED          = 10002;
    static final int KEY_TICK_STOPPED          = 10003;
    static final int KEY_BPM_CHANGED           = 10004;
    static final int KEY_TODAY_MINUTES_REQUEST = 10005;
    static final int KEY_TODAY_MINUTES         = 10006;
    static final int KEY_WEEK_MINUTES          = 10007;
    static final int KEY_RECORDING_STATE       = 10008;
    static final int KEY_MARKER                = 10009;
    static final int KEY_STOP_RECORDING        = 10010;
    static final int KEY_START_RECORDING       = 10011;

    // Glance message key for the metronome chip — must match the index of
    // METRONOME_MINUTES_TODAY in the top-level package.json messageKeys list.
    static final int KEY_GLANCE_METRONOME_MINUTES = 10017;

    static final String EXTRA_EVENT = "event";
    static final String EXTRA_VALUE = "value";

    // Intent EXTRA_EVENT values.
    static final String EVT_OPENED   = "opened";
    static final String EVT_CLOSED   = "closed";
    static final String EVT_TICK_ON  = "tick_on";
    static final String EVT_TICK_OFF = "tick_off";
    static final String EVT_BPM      = "bpm";
    static final String EVT_QUERY    = "query";
    static final String EVT_MARKER   = "marker";
    static final String EVT_REC_STOP = "rec_stop";
    static final String EVT_REC_START = "rec_start";

    // NotificationChannel settings are immutable after creation, so to lift
    // importance from LOW → DEFAULT we have to use a new id. Bump this
    // string whenever the channel needs re-creation.
    private static final String CHANNEL_ID = "metronome_session_v2";
    private static final int NOTIF_ID = 1001;

    private static final String PREFS = "metronome";
    // Per-day key prefix; e.g. "secs_2026-05-15" → int seconds of ticking.
    private static final String SECS_PREFIX = "secs_";

    private MediaRecorder recorder;
    private File inProgressFile;
    private File inProgressSidecar;
    private long sessionStartMs;     // when the current app-open began
    // Recording lifecycle is no longer 1:1 with app-open — DOWN stops it,
    // SELECT (metronome start) restarts it, so a session can produce multiple
    // .m4a files. These track the *currently in-flight* recording.
    private long recordingStartMs;   // 0 when no MediaRecorder is running
    private int  recordingBpmMin = -1; // -1 until first BPM seen this recording
    private int  recordingBpmMax = -1;

    private long currentTickStartMs; // 0 when not ticking
    private int  currentBpm = -1;

    // Event log mirroring the recording, written to <basename>.json on close.
    // Powers the BPM-vs-time graph in the offline audio editor.
    private StringBuilder sidecarEvents = new StringBuilder();

    // Bookkeeping for the every-30s flush. Without this, an unclean process
    // death (battery saver kill, force-stop) drops the entire current
    // session's accumulated minutes. With it, the worst case is ~30s lost.
    private static final long FLUSH_INTERVAL_MS = 30_000L;
    private long lastFlushMs;
    private Handler flushHandler;
    private Runnable flushTask;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Critical: MediaListenerService called startForegroundService(), so
        // we MUST call startForeground() within ~5s on every code path —
        // including when intent==null (system restart) or when the very
        // first event after a process restart is a non-OPEN event like
        // TICK_ON. Failing this watchdog throws ForegroundServiceDidNotStart
        // InTimeException and the entire app process is killed, which then
        // restarts MediaListenerService, which gets another stale event,
        // and we crash again. Calling startForegroundNotif() *here* breaks
        // the loop regardless of event order.
        try {
            startForegroundNotif();
        } catch (Throwable t) {
            Log.e(TAG, "startForeground failed; bailing", t);
            stopSelf();
            return START_NOT_STICKY;
        }

        if (intent == null) {
            // System-initiated restart with no event payload — nothing useful
            // to do until the watch sends a fresh event.
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        String evt = intent.getStringExtra(EXTRA_EVENT);
        int value = intent.getIntExtra(EXTRA_VALUE, 0);
        Log.i(TAG, "evt=" + evt + " value=" + value);
        if (evt == null) {
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }

        // If a process restart leaves us with no session state but the
        // watch is still mid-session, the first event we'll see is BPM /
        // TICK_ON / TICK_OFF — promote that into a synthetic OPEN so the
        // recording can recover instead of dropping the rest of the
        // session on the floor.
        if (sessionStartMs == 0 && !EVT_OPENED.equals(evt) && !EVT_CLOSED.equals(evt)
                && !EVT_MARKER.equals(evt) && !EVT_REC_STOP.equals(evt)
                && !EVT_REC_START.equals(evt)) {
            Log.w(TAG, "no session state; synthesizing OPEN from " + evt);
            handleOpened(value > 0 ? value : 100);
        }

        switch (evt) {
            case EVT_OPENED:   handleOpened(value);    break;
            case EVT_BPM:      handleBpm(value);       break;
            case EVT_TICK_ON:  handleTickStart(value); break;
            case EVT_TICK_OFF: handleTickStop();       break;
            case EVT_QUERY:    sendTotalsToWatch();    break;
            case EVT_MARKER:   handleMarker();         break;
            case EVT_REC_STOP: handleRecStop();        break;
            case EVT_REC_START: handleRecStart();      break;
            case EVT_CLOSED:   handleClosed();         break;
        }
        return START_NOT_STICKY;
    }

    private void handleOpened(int initialBpm) {
        sessionStartMs = System.currentTimeMillis();
        currentBpm = initialBpm;
        currentTickStartMs = 0;
        // lastFlushMs is the *tick-run anchor*, not a session anchor —
        // intentionally left at 0 here so idle setup time doesn't count.
        // handleTickStart() sets it; handleTickStop()/flushTask consume it.
        lastFlushMs = 0;
        // startForeground is already called by onStartCommand before we get
        // here, so no need to do it again.
        startRecording();
        logEvent("open", initialBpm);
        // Practice-time accounting: ONLY ticking-active stretches count
        // toward today. flushAccumulatedSeconds() is a no-op when not
        // ticking; the 30s periodic flush below caps loss to one interval
        // if the process dies mid-tick.
        scheduleFlush();
        sendTotalsToWatch();
    }

    private void scheduleFlush() {
        if (flushHandler == null) {
            flushHandler = new Handler(Looper.getMainLooper());
        }
        if (flushTask != null) flushHandler.removeCallbacks(flushTask);
        flushTask = new Runnable() {
            @Override public void run() {
                if (sessionStartMs == 0) return;
                flushAccumulatedSeconds();
                scheduleFlush(); // re-arm
            }
        };
        flushHandler.postDelayed(flushTask, FLUSH_INTERVAL_MS);
    }

    private void cancelFlush() {
        if (flushHandler != null && flushTask != null) {
            flushHandler.removeCallbacks(flushTask);
        }
        flushTask = null;
    }

    /**
     * Commit whole-second deltas of *ticking time* (only) since the last
     * flush boundary into today's tally and shift {@link #lastFlushMs}
     * forward by exactly that many seconds. Idle stretches between tick
     * runs don't count — lastFlushMs is 0 while not ticking, which makes
     * this method a no-op. Working in whole seconds preserves sub-second
     * remainders so back-to-back short flushes don't lose fractional time.
     */
    private void flushAccumulatedSeconds() {
        if (currentTickStartMs == 0 || lastFlushMs == 0) return;
        long now = System.currentTimeMillis();
        int secs = (int) ((now - lastFlushMs) / 1000L);
        if (secs > 0) {
            addSecondsToToday(secs);
            lastFlushMs += secs * 1000L;
            sendTotalsToWatch();
            sendMinutesToGlance();
        }
    }

    private void handleBpm(int bpm) {
        currentBpm = bpm;
        if (recordingStartMs > 0) {
            if (recordingBpmMin < 0 || bpm < recordingBpmMin) recordingBpmMin = bpm;
            if (recordingBpmMax < 0 || bpm > recordingBpmMax) recordingBpmMax = bpm;
        }
        logEvent("bpm", bpm);
    }

    private void handleTickStart(int bpm) {
        if (bpm > 0) currentBpm = bpm;
        // Starting the metronome should always produce a recording — if the
        // user previously hit STOP_RECORDING via the down button, this gets
        // them back to recording without needing a separate "start" button.
        if (recorder == null) startRecording();
        if (bpm > 0) handleBpm(bpm);
        currentTickStartMs = System.currentTimeMillis();
        // Anchor the flush window to *this* tick run so today/week totals
        // only accumulate ticking time.
        lastFlushMs = currentTickStartMs;
        logEvent("tick_start", bpm > 0 ? bpm : currentBpm);
    }

    private void handleTickStop() {
        if (currentTickStartMs > 0) {
            // Commit the tail seconds since the last periodic flush before
            // clearing the anchor — otherwise sub-30s runs would lose their
            // whole contribution.
            flushAccumulatedSeconds();
            currentTickStartMs = 0;
            lastFlushMs = 0;
            logEvent("tick_stop", -1);
            sendTotalsToWatch();
        }
    }

    private void handleMarker() {
        if (recordingStartMs == 0) {
            // No active recording — a marker would have nothing to attach to,
            // so silently drop it rather than starting a recording just to
            // hold one event.
            Log.w(TAG, "marker dropped: no active recording");
            return;
        }
        logEvent("marker", -1);
    }

    private void handleRecStop() {
        // Idempotent: if there's no active recording, this is a no-op. The
        // *session* keeps running either way — practice-time accounting and
        // the foreground notification are tied to sessionStartMs, not the
        // recording lifecycle.
        if (recorder != null) stopRecording();
        // Refresh the foreground notification so its "Recording" / "paused"
        // text matches the new state. startForegroundNotif() with the same
        // NOTIF_ID just updates the existing notification.
        startForegroundNotif();
    }

    private void handleRecStart() {
        // Idempotent companion to handleRecStop: a DOWN tap on the watch lands
        // here when the user wants to re-arm recording after a deliberate stop
        // (without having to start the metronome to do it). If a recording is
        // already running, startRecording() short-circuits inside.
        startRecording();
        startForegroundNotif();
    }

    private void handleClosed() {
        // Treat a close-while-ticking as a graceful tick-stop first — that
        // path flushes the tail seconds for the in-progress run, so no
        // extra flush is needed here.
        if (currentTickStartMs > 0) handleTickStop();
        logEvent("close", -1);
        cancelFlush();
        // stopRecording() needs sessionStartMs to build the final filename
        // and sidecar — zero it out only after the rename is done.
        stopRecording();
        sessionStartMs = 0;
        lastFlushMs = 0;
        sendTotalsToWatch();
        sendMinutesToGlance();
        stopForeground(true);
        stopSelf();
    }

    private void logEvent(String type, int bpm) {
        // Events are sidecar entries for the current recording's audio file.
        // No active recording → nowhere to put them; drop instead of buffering.
        if (recordingStartMs == 0) return;
        long tMs = System.currentTimeMillis() - recordingStartMs;
        if (sidecarEvents.length() > 0) sidecarEvents.append(",\n  ");
        sidecarEvents.append("{\"t_ms\":").append(tMs)
                     .append(",\"type\":\"").append(type).append("\"");
        if (bpm > 0) sidecarEvents.append(",\"bpm\":").append(bpm);
        sidecarEvents.append("}");
    }

    // === Recording =======================================================

    private File metronomeDir() {
        File base = getExternalFilesDir(Environment.DIRECTORY_MUSIC);
        File dir = new File(base, "metronome");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    private String startTimestamp() {
        return new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US)
                .format(new Date(recordingStartMs));
    }

    private void startRecording() {
        if (recorder != null) return; // already recording
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "RECORD_AUDIO not granted — skipping recording");
            return;
        }
        // Each recording is its own file with its own timeline — reset state
        // that was carried over from any previous recording in this session.
        recordingStartMs = System.currentTimeMillis();
        recordingBpmMin = currentBpm > 0 ? currentBpm : -1;
        recordingBpmMax = currentBpm > 0 ? currentBpm : -1;
        sidecarEvents.setLength(0);
        try {
            String basename = startTimestamp() + "_pending";
            inProgressFile = new File(metronomeDir(), basename + ".m4a");
            inProgressSidecar = new File(metronomeDir(), basename + ".json");
            // The no-arg MediaRecorder constructor was deprecated in API 31
            // in favor of one that takes a Context for foreground-service
            // attribution. minSdk is 21, so version-gate.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                recorder = new MediaRecorder(this);
            } else {
                recorder = new MediaRecorder();
            }
            recorder.setAudioSource(MediaRecorder.AudioSource.MIC);
            recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
            recorder.setAudioEncodingBitRate(64_000);
            recorder.setAudioSamplingRate(44_100);
            recorder.setAudioChannels(1);
            recorder.setOutputFile(inProgressFile.getAbsolutePath());
            recorder.prepare();
            recorder.start();
            Log.i(TAG, "recording → " + inProgressFile.getAbsolutePath());
            sendRecordingStateToWatch(true);
        } catch (Exception e) {
            Log.e(TAG, "MediaRecorder start failed", e);
            try { if (recorder != null) recorder.release(); } catch (Throwable ignored) {}
            recorder = null;
            inProgressFile = null;
            recordingStartMs = 0;
            recordingBpmMin = -1;
            recordingBpmMax = -1;
            sendRecordingStateToWatch(false);
        }
    }

    private void stopRecording() {
        if (recorder == null) return;
        try {
            recorder.stop();
        } catch (Exception e) {
            Log.w(TAG, "MediaRecorder.stop() failed (likely too-short recording)", e);
        }
        try { recorder.reset();   } catch (Throwable ignored) {}
        try { recorder.release(); } catch (Throwable ignored) {}
        recorder = null;
        sendRecordingStateToWatch(false);
        if (inProgressFile != null && inProgressFile.exists()) {
            String range;
            if (recordingBpmMin < 0) {
                range = "unknown";
            } else if (recordingBpmMin == recordingBpmMax) {
                range = recordingBpmMin + "bpm";
            } else {
                range = recordingBpmMin + "-" + recordingBpmMax + "bpm";
            }
            String finalBase = startTimestamp() + "_" + range;
            File renamedAudio = new File(inProgressFile.getParentFile(),
                    finalBase + ".m4a");
            File renamedSidecar = new File(inProgressFile.getParentFile(),
                    finalBase + ".json");
            if (!inProgressFile.renameTo(renamedAudio)) {
                Log.w(TAG, "audio rename failed; leaving as " + inProgressFile.getName());
                renamedAudio = inProgressFile;
            }
            // Flush the event log to the sidecar path that pairs with the
            // (possibly-renamed) audio file. We write *now* instead of
            // streaming during the session because (a) the event volume is
            // tiny and (b) we don't want stale partials if the service is
            // killed mid-recording — a missing sidecar is clearer than a
            // truncated one.
            try (FileWriter w = new FileWriter(renamedSidecar)) {
                w.write("{\n");
                w.write("  \"started_at_unix_ms\": " + recordingStartMs + ",\n");
                w.write("  \"bpm_min\": " + recordingBpmMin + ",\n");
                w.write("  \"bpm_max\": " + recordingBpmMax + ",\n");
                w.write("  \"events\": [\n  ");
                w.write(sidecarEvents.toString());
                w.write("\n  ]\n");
                w.write("}\n");
            } catch (Exception e) {
                Log.e(TAG, "sidecar write failed", e);
            }
            if (inProgressSidecar != null && inProgressSidecar.exists()) {
                inProgressSidecar.delete();
            }
        }
        inProgressFile = null;
        inProgressSidecar = null;
        recordingStartMs = 0;
        recordingBpmMin = -1;
        recordingBpmMax = -1;
    }

    // === Daily tally =====================================================

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private static String dateKey(Calendar cal) {
        return SECS_PREFIX + new SimpleDateFormat("yyyy-MM-dd", Locale.US)
                .format(cal.getTime());
    }

    private void addSecondsToToday(int secs) {
        SharedPreferences p = prefs();
        String key = dateKey(Calendar.getInstance());
        int existing = p.getInt(key, 0);
        p.edit().putInt(key, existing + secs).apply();
    }

    /**
     * Today's accumulated minutes. Round-UP from seconds: any nonzero
     * ticking today reads as ≥1 minute on the watchface chip and the
     * watchapp stats line. Truncation made 25s look like "no activity",
     * which is misleading at a glance.
     */
    int todayMinutes() {
        int secs = prefs().getInt(dateKey(Calendar.getInstance()), 0);
        return secs <= 0 ? 0 : (secs + 59) / 60;
    }

    /** Rolling 7-day window including today (round-up, same rationale). */
    int weekMinutes() {
        int total = 0;
        Calendar c = Calendar.getInstance();
        for (int i = 0; i < 7; i++) {
            total += prefs().getInt(dateKey(c), 0);
            c.add(Calendar.DAY_OF_YEAR, -1);
        }
        return total <= 0 ? 0 : (total + 59) / 60;
    }

    // === Watch / Glance messaging ========================================

    private void sendTotalsToWatch() {
        if (!PebbleKit.isWatchConnected(this)) return;
        PebbleDictionary d = new PebbleDictionary();
        d.addInt32(KEY_TODAY_MINUTES, todayMinutes());
        d.addInt32(KEY_WEEK_MINUTES,  weekMinutes());
        PebbleKit.sendDataToPebble(this, METRONOME_UUID, d);
    }

    private void sendRecordingStateToWatch(boolean on) {
        if (!PebbleKit.isWatchConnected(this)) return;
        PebbleDictionary d = new PebbleDictionary();
        d.addInt32(KEY_RECORDING_STATE, on ? 1 : 0);
        PebbleKit.sendDataToPebble(this, METRONOME_UUID, d);
    }

    private void sendMinutesToGlance() {
        if (!PebbleKit.isWatchConnected(this)) return;
        PebbleDictionary d = new PebbleDictionary();
        d.addInt32(KEY_GLANCE_METRONOME_MINUTES, todayMinutes());
        PebbleKit.sendDataToPebble(this, GLANCE_UUID, d);
    }

    /** Static helper for Glance's REQUEST_REFRESH path. Round-up matches the
     *  instance method so the chip and watchapp agree. */
    static int peekTodayMinutes(Context ctx) {
        SharedPreferences p = ctx.getSharedPreferences(PREFS, MODE_PRIVATE);
        int secs = p.getInt(SECS_PREFIX + new SimpleDateFormat(
                "yyyy-MM-dd", Locale.US).format(new Date()), 0);
        return secs <= 0 ? 0 : (secs + 59) / 60;
    }

    static int peekTodaySeconds(Context ctx) {
        SharedPreferences p = ctx.getSharedPreferences(PREFS, MODE_PRIVATE);
        return p.getInt(SECS_PREFIX + new SimpleDateFormat(
                "yyyy-MM-dd", Locale.US).format(new Date()), 0);
    }

    static boolean isCurrentlyRecording(Context ctx) {
        // The phone-side notion of "is recording right now" is just "does
        // the most-recent file in metronomeDir match the *_pending pattern."
        // No IPC needed — MainActivity polls the filesystem on each onResume.
        File dir = new File(ctx.getExternalFilesDir(Environment.DIRECTORY_MUSIC),
                "metronome");
        if (!dir.exists()) return false;
        File[] entries = dir.listFiles();
        if (entries == null) return false;
        for (File f : entries) {
            if (f.getName().endsWith("_pending.m4a")) return true;
        }
        return false;
    }

    // === Foreground notification =========================================

    private void startForegroundNotif() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        // IMPORTANCE_DEFAULT (not LOW): on Android 16 / Pixel, LOW-importance
        // ongoing notifications land under the collapsed "Silent" section and
        // are easy to miss. We don't want sound — Notification.Builder
        // suppresses sound on foreground-service notifications by default, so
        // DEFAULT just affects visibility, not audibility.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                && nm.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL_ID,
                    "Metronome session", NotificationManager.IMPORTANCE_DEFAULT);
            ch.setDescription("Active while the Pebble metronome is open");
            ch.setSound(null, null);
            ch.enableVibration(false);
            nm.createNotificationChannel(ch);
        }
        String contentText;
        if (recordingStartMs > 0) {
            int elapsedSec = (int) ((System.currentTimeMillis() - recordingStartMs) / 1000);
            contentText = "Recording (" + elapsedSec + "s) · BPM "
                    + (recordingBpmMin < 0 ? "?" :
                       recordingBpmMin == recordingBpmMax ? String.valueOf(recordingBpmMin)
                           : (recordingBpmMin + "–" + recordingBpmMax));
        } else if (sessionStartMs > 0) {
            contentText = "Session active (recording paused)";
        } else {
            contentText = "Recording from mic while watch app is open";
        }
        Notification notif = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("Glance metronome recording")
                .setContentText(contentText)
                .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setTicker("Glance metronome recording")
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, notif,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
        } else {
            startForeground(NOTIF_ID, notif);
        }
    }
}
