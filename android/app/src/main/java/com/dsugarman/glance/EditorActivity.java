package com.dsugarman.glance;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Hosts the audio editor (same HTML/JS as tools/audio-editor/) in a WebView.
 * The static files and the /api/files + /recordings/ endpoints come from
 * an in-process {@link EditorServer} bound to 127.0.0.1.
 */
public class EditorActivity extends Activity {

    private static final String TAG = "GlanceEditor";

    private EditorServer server;
    private WebView webView;

    @Override
    @SuppressWarnings("SetJavaScriptEnabled")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // No XML layout — we wire up a WebView in code so this stays a
        // single file. FrameLayout lets us overlay an error TextView if
        // the server fails to start.
        FrameLayout root = new FrameLayout(this);
        webView = new WebView(this);
        root.addView(webView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);

        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        // Required so WebView treats audio/* MIME from localhost as media
        // that can be decoded by Web Audio API without user gesture.
        ws.setMediaPlaybackRequiresUserGesture(false);
        // Expose the share-sheet bridge under window.GlanceBridge so the
        // editor JS can invoke Android intents (otherwise the ⬇ export
        // button is a silent no-op inside WebView).
        webView.addJavascriptInterface(new EditorBridge(this), "GlanceBridge");
        webView.setWebViewClient(new WebViewClient());
        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(android.webkit.ConsoleMessage cm) {
                Log.d(TAG, "JS: " + cm.message() + " @ " + cm.sourceId() + ":" + cm.lineNumber());
                return true;
            }
        });
        // WebView's default download path is "do nothing." For our "save
        // selection as WAV" button we route through a POST /api/save-clip
        // endpoint instead of a browser download, but if a future feature
        // ever does need a download we'd hook it here.

        try {
            server = new EditorServer();
            server.start(this);
            String url = "http://127.0.0.1:" + server.port() + "/";
            Log.i(TAG, "loading " + url);
            webView.loadUrl(url);
        } catch (Exception e) {
            Log.e(TAG, "server start failed", e);
            TextView err = new TextView(this);
            err.setText("Editor server failed to start: " + e.getMessage());
            err.setPadding(40, 40, 40, 40);
            root.addView(err);
            webView.setVisibility(View.GONE);
        }
    }

    @Override
    protected void onDestroy() {
        if (webView != null) {
            webView.loadUrl("about:blank");
            webView.destroy();
        }
        if (server != null) {
            server.stop();
        }
        super.onDestroy();
    }
}
