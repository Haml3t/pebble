package com.dsugarman.glance;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;
import android.webkit.JavascriptInterface;

/**
 * JavaScript ↔ Android bridge exposed to the audio-editor WebView as
 * {@code window.GlanceBridge}. The editor JS calls into this when it
 * wants to do something the browser can't do alone — currently just
 * "share/export a recording via the Android share sheet."
 *
 * Methods annotated with @JavascriptInterface run on a non-UI thread,
 * so anything touching the activity (e.g. startActivity) is bounced
 * back to the main thread.
 */
public class EditorBridge {
    private static final String TAG = "GlanceBridge";
    private final Activity activity;

    EditorBridge(Activity activity) {
        this.activity = activity;
    }

    /** Returns "true" if Android can be asked to start activities from this
     *  bridge — i.e. the bridge exists. JS feature-detects so the laptop
     *  Python server (where this object isn't injected) keeps using the
     *  browser-native download path. */
    @JavascriptInterface
    public boolean isAvailable() { return true; }

    /**
     * Open Android's share chooser for the given recordings-dir filename.
     * Returns true if the chooser was launched; false on any error (file
     * missing, malformed name, etc.). Errors are surfaced to the JS side
     * via the boolean — the JS then falls back to the "open Files app"
     * status message.
     */
    @JavascriptInterface
    public boolean shareFile(final String name) {
        if (activity == null || name == null) return false;
        try {
            final Uri uri = MetronomeFileProvider.uriFor(name);
            activity.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    Intent send = new Intent(Intent.ACTION_SEND);
                    send.setType(activity.getContentResolver().getType(uri));
                    send.putExtra(Intent.EXTRA_STREAM, uri);
                    send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    Intent chooser = Intent.createChooser(send, "Share " + name);
                    chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    try {
                        activity.startActivity(chooser);
                    } catch (Throwable t) {
                        Log.e(TAG, "share chooser failed", t);
                    }
                }
            });
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "shareFile failed", t);
            return false;
        }
    }
}
