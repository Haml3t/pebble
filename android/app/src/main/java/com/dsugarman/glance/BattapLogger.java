package com.dsugarman.glance;

import android.content.Context;
import android.util.Log;

import com.getpebble.android.kit.PebbleKit;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;

/**
 * Consumer for the Glance watchface's battery_tap DataLogging session.
 *
 * The watch buffers rows on-device (up to ~640 KB) and the Pebble companion
 * app drains them to here whenever the BT connection is up. We append each
 * row to a CSV in the app's private external files dir so it's reachable
 * via {@code adb pull /sdcard/Android/data/com.dsugarman.glance/files/battap.csv}.
 *
 * The wire format is a 46-byte packed struct produced by
 * {@code battery_tap.c::emit_sample}; column layout is mirrored here in
 * {@link #appendRow}. Update both sides if the C struct changes.
 */
public class BattapLogger {
    private static final String TAG = "GlanceBattap";

    // Matches BATTAP_DATALOG_TAG in src/c/battery_tap.c. Changing it
    // orphans any rows still buffered on the watch.
    private static final long BATTAP_TAG = 0xBA77AB01L;

    private static final String FILENAME = "battap.csv";

    public static PebbleKit.PebbleDataLogReceiver newReceiver(final UUID watchUuid) {
        return new PebbleKit.PebbleDataLogReceiver(watchUuid) {
            @Override
            public void receiveData(Context ctx, UUID logUuid, Long timestamp,
                                    Long tag, byte[] data) {
                if (tag == null || tag.longValue() != BATTAP_TAG) return;
                if (data == null || data.length < 46) {
                    Log.w(TAG, "short BATTAP row: "
                            + (data == null ? "null" : data.length));
                    return;
                }
                appendRow(ctx, data);
            }

            @Override
            public void onFinishSession(Context ctx, UUID logUuid,
                                        Long timestamp, Long tag) {
                if (tag != null && tag.longValue() == BATTAP_TAG) {
                    Log.i(TAG, "session finished");
                }
            }
        };
    }

    /**
     * Append a 46-byte BattapRow to the CSV. Public so the AppMessage path
     * in {@link MediaListenerService} can dispatch here when a BATTAP_BLOB
     * lands (parallel to the DataLog receiver path above).
     */
    public static void appendRow(Context ctx, byte[] data) {
        // Mirror of struct BattapRow in src/c/battery_tap.c. Pebble is
        // little-endian (STM32 ARM Cortex-M). The struct is __packed so
        // there's no padding to skip.
        ByteBuffer bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        long epoch       = bb.getInt() & 0xFFFFFFFFL;
        int  reason      = bb.get() & 0xFF;
        int  pct         = bb.get() & 0xFF;
        int  isCharging  = bb.get() & 0xFF;
        int  isPlugged   = bb.get() & 0xFF;
        long uptime      = bb.getInt() & 0xFFFFFFFFL;
        int  hrPeriod    = bb.getShort() & 0xFFFF;
        long hrSamples   = bb.getInt() & 0xFFFFFFFFL;
        long hrFast      = bb.getInt() & 0xFFFFFFFFL;
        long hrSlow      = bb.getInt() & 0xFFFFFFFFL;
        long amTx        = bb.getInt() & 0xFFFFFFFFL;
        long amRx        = bb.getInt() & 0xFFFFFFFFL;
        long btUp        = bb.getInt() & 0xFFFFFFFFL;
        long tap         = bb.getInt() & 0xFFFFFFFFL;
        int  sleep       = bb.getInt();           // signed: -1 sentinel

        File dir = ctx.getExternalFilesDir(null);
        if (dir == null) {
            Log.e(TAG, "no external files dir");
            return;
        }
        File file = new File(dir, FILENAME);
        boolean needHeader = !file.exists();

        try (BufferedWriter w = new BufferedWriter(new FileWriter(file, true))) {
            if (needHeader) {
                w.write("epoch,reason,pct,chg,plg,uptime,hrp,hrs,hrfast,hrslow,tx,rx,btup,tap,slp\n");
            }
            w.write(String.format(
                    "%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                    epoch, reasonStr(reason), pct, isCharging, isPlugged,
                    uptime, hrPeriod, hrSamples, hrFast, hrSlow,
                    amTx, amRx, btUp, tap, sleep));
        } catch (IOException e) {
            Log.e(TAG, "write failed", e);
        }
    }

    private static String reasonStr(int r) {
        switch (r) {
            case 0: return "init";
            case 1: return "deinit";
            case 2: return "batt";
            case 3: return "tick";
            default: return "?";
        }
    }
}
