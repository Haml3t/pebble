package com.dsugarman.glance;

import android.content.Context;
import android.os.Environment;
import android.util.Log;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Minimal HTTP/1.1 server hosted in-process so the audio editor (the same
 * HTML/JS that {@code tools/audio-editor/server.py} serves on a laptop) can
 * run inside a WebView on the phone. Binds to 127.0.0.1 on an OS-assigned
 * port — discoverable via {@link #port()} after {@link #start(Context)}.
 *
 * Routes:
 *   GET  /                       → assets/audio-editor/index.html
 *   GET  /app.js, /style.css     → corresponding asset
 *   GET  /api/files              → JSON listing of metronomeDir m4a/json
 *   GET  /recordings/<name>      → file from metronomeDir
 *   POST /api/save-clip?name=X   → write request body to metronomeDir/X
 *
 * Intentionally NOT thread-pooled: each request is handled serially in a
 * background thread. Editor traffic is tiny (a few recordings, fetched
 * once on load) and the server is short-lived (only while EditorActivity
 * is up), so the simpler model is correct.
 */
class EditorServer implements Runnable {
    private static final String TAG = "EditorServer";
    private static final Pattern REQ_LINE = Pattern.compile(
            "^(GET|POST) (\\S+) HTTP/1\\.[01]$");

    private ServerSocket socket;
    private Thread thread;
    private Context appContext;

    int port() {
        return socket == null ? -1 : socket.getLocalPort();
    }

    void start(Context ctx) throws IOException {
        appContext = ctx.getApplicationContext();
        // Pin port 8765 (matches the laptop server) so it's reachable from
        // the phone's regular Chrome too, not just the in-app WebView.
        // Fall back to an OS-assigned port if 8765 is already in use.
        try {
            socket = new ServerSocket(8765, 8, InetAddress.getByName("127.0.0.1"));
        } catch (IOException e) {
            Log.w(TAG, "port 8765 in use; falling back to ephemeral", e);
            socket = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
        }
        thread = new Thread(this, "EditorServer");
        thread.setDaemon(true);
        thread.start();
        Log.i(TAG, "listening on http://127.0.0.1:" + port());
    }

    void stop() {
        try { if (socket != null) socket.close(); } catch (IOException ignored) {}
        if (thread != null) thread.interrupt();
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted() && !socket.isClosed()) {
            try {
                Socket client = socket.accept();
                try {
                    handle(client);
                } finally {
                    try { client.close(); } catch (IOException ignored) {}
                }
            } catch (IOException e) {
                if (!socket.isClosed()) Log.w(TAG, "accept failed", e);
                return;
            }
        }
    }

    private File metronomeDir() {
        File base = appContext.getExternalFilesDir(Environment.DIRECTORY_MUSIC);
        File dir = new File(base, "metronome");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    // Star list is persisted as a single JSON sidecar in the recordings dir.
    // Mirrors the Python server's metadata.json so the same files are
    // compatible between the on-phone and on-laptop editors.
    private File metaFile() { return new File(metronomeDir(), "metadata.json"); }

    /** Aggregate metadata persisted to metronome/metadata.json. Schema matches
     *  the Python server: starred list + display-name + decoded-duration maps. */
    private static class Meta {
        java.util.HashSet<String> starred = new java.util.HashSet<>();
        java.util.HashMap<String, String> names = new java.util.HashMap<>();
        java.util.HashMap<String, Double> durations = new java.util.HashMap<>();
    }

    private Meta loadMeta() {
        Meta m = new Meta();
        File f = metaFile();
        if (!f.exists()) return m;
        try (java.io.FileReader fr = new java.io.FileReader(f)) {
            java.io.BufferedReader br = new java.io.BufferedReader(fr);
            StringBuilder sb = new StringBuilder();
            String line; while ((line = br.readLine()) != null) sb.append(line);
            String body = sb.toString();
            // starred array
            Matcher sm = Pattern.compile("\"starred\"\\s*:\\s*\\[(.*?)\\]",
                    Pattern.DOTALL).matcher(body);
            if (sm.find()) {
                Matcher nm = Pattern.compile("\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(sm.group(1));
                while (nm.find()) m.starred.add(unescapeJsonString(nm.group(1)));
            }
            // names object — {"foo.m4a": "Display name", ...}
            Matcher nmObj = Pattern.compile("\"names\"\\s*:\\s*\\{(.*?)\\}",
                    Pattern.DOTALL).matcher(body);
            if (nmObj.find()) {
                Matcher kv = Pattern.compile(
                        "\"((?:[^\"\\\\]|\\\\.)*)\"\\s*:\\s*" +
                        "\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(nmObj.group(1));
                while (kv.find()) {
                    m.names.put(unescapeJsonString(kv.group(1)),
                                unescapeJsonString(kv.group(2)));
                }
            }
            // durations object — {"foo.m4a": 422.5, ...}
            Matcher dObj = Pattern.compile("\"durations\"\\s*:\\s*\\{(.*?)\\}",
                    Pattern.DOTALL).matcher(body);
            if (dObj.find()) {
                Matcher kv = Pattern.compile(
                        "\"((?:[^\"\\\\]|\\\\.)*)\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)")
                    .matcher(dObj.group(1));
                while (kv.find()) {
                    try {
                        m.durations.put(unescapeJsonString(kv.group(1)),
                                        Double.parseDouble(kv.group(2)));
                    } catch (NumberFormatException ignored) {}
                }
            }
        } catch (IOException ignored) {}
        return m;
    }

    private void saveMeta(Meta m) {
        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        // starred
        java.util.List<String> sortedStars = new ArrayList<>(m.starred);
        java.util.Collections.sort(sortedStars);
        sb.append("  \"starred\": [");
        boolean first = true;
        for (String s : sortedStars) {
            if (!first) sb.append(",");
            first = false;
            sb.append("\n    \"").append(escapeJsonString(s)).append("\"");
        }
        if (!sortedStars.isEmpty()) sb.append("\n  ");
        sb.append("],\n");
        // names
        java.util.List<String> sortedNames = new ArrayList<>(m.names.keySet());
        java.util.Collections.sort(sortedNames);
        sb.append("  \"names\": {");
        first = true;
        for (String k : sortedNames) {
            if (!first) sb.append(",");
            first = false;
            sb.append("\n    \"").append(escapeJsonString(k)).append("\": \"")
              .append(escapeJsonString(m.names.get(k))).append("\"");
        }
        if (!sortedNames.isEmpty()) sb.append("\n  ");
        sb.append("},\n");
        // durations
        java.util.List<String> sortedDurs = new ArrayList<>(m.durations.keySet());
        java.util.Collections.sort(sortedDurs);
        sb.append("  \"durations\": {");
        first = true;
        for (String k : sortedDurs) {
            if (!first) sb.append(",");
            first = false;
            sb.append("\n    \"").append(escapeJsonString(k)).append("\": ")
              .append(m.durations.get(k));
        }
        if (!sortedDurs.isEmpty()) sb.append("\n  ");
        sb.append("}\n}\n");
        try (FileOutputStream fos = new FileOutputStream(metaFile())) {
            fos.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "saveMeta failed", e);
        }
    }

    private void handle(Socket client) {
        try {
            InputStream rawIn = client.getInputStream();
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(rawIn, StandardCharsets.UTF_8));
            String line = reader.readLine();
            if (line == null) return;
            Matcher m = REQ_LINE.matcher(line);
            if (!m.matches()) {
                writeStatus(client, 400, "Bad Request");
                return;
            }
            String method = m.group(1);
            String pathWithQuery = m.group(2);
            int qIdx = pathWithQuery.indexOf('?');
            String path = qIdx < 0 ? pathWithQuery : pathWithQuery.substring(0, qIdx);
            String query = qIdx < 0 ? "" : pathWithQuery.substring(qIdx + 1);

            int contentLength = 0;
            String hdr;
            while ((hdr = reader.readLine()) != null && !hdr.isEmpty()) {
                int c = hdr.indexOf(':');
                if (c > 0 && hdr.substring(0, c).trim().equalsIgnoreCase("Content-Length")) {
                    try { contentLength = Integer.parseInt(hdr.substring(c + 1).trim()); }
                    catch (NumberFormatException ignored) {}
                }
            }

            if ("GET".equals(method)) {
                routeGet(client, path, query);
            } else if ("POST".equals(method)) {
                routePost(client, path, query, rawIn, reader, contentLength);
            } else {
                writeStatus(client, 405, "Method Not Allowed");
            }
        } catch (Exception e) {
            Log.w(TAG, "handle failed", e);
        }
    }

    // ---- GET routes -----------------------------------------------------

    private void routeGet(Socket client, String path, String query) throws IOException {
        if (path.equals("/") || path.equals("/index.html")) {
            serveAsset(client, "audio-editor/index.html", "text/html; charset=utf-8");
        } else if (path.equals("/app.js")) {
            serveAsset(client, "audio-editor/app.js", "application/javascript; charset=utf-8");
        } else if (path.equals("/style.css")) {
            serveAsset(client, "audio-editor/style.css", "text/css; charset=utf-8");
        } else if (path.equals("/api/files")) {
            serveFileList(client);
        } else if (path.equals("/api/markers")) {
            getMarkers(client, query);
        } else if (path.startsWith("/recordings/")) {
            String name = path.substring("/recordings/".length());
            serveRecording(client, name);
        } else {
            writeStatus(client, 404, "Not Found");
        }
    }

    private void serveAsset(Socket client, String assetPath, String contentType) throws IOException {
        try (InputStream in = appContext.getAssets().open(assetPath)) {
            byte[] body = readAll(in);
            writeResponse(client, 200, "OK", contentType, body);
        } catch (IOException e) {
            writeStatus(client, 404, "Not Found");
        }
    }

    private void serveFileList(Socket client) throws IOException {
        Meta meta = loadMeta();
        java.util.HashSet<String> starred = meta.starred;
        java.util.HashMap<String, String> names = meta.names;
        java.util.HashMap<String, Double> durations = meta.durations;
        StringBuilder json = new StringBuilder("[");
        File dir = metronomeDir();
        File[] entries = dir.listFiles();
        if (entries != null) {
            List<File> m4as = new ArrayList<>();
            for (File f : entries) if (f.getName().endsWith(".m4a")
                    && !f.getName().endsWith("_pending.m4a")) m4as.add(f);
            Collections.sort(m4as, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
            boolean first = true;
            for (File f : m4as) {
                if (!first) json.append(",");
                first = false;
                String stem = f.getName().substring(0, f.getName().length() - 4);
                File sidecar = new File(dir, stem + ".json");
                Integer lo = null, hi = null;
                Matcher pm = Pattern.compile("_(\\d+)(?:-(\\d+))?bpm").matcher(stem);
                if (pm.find()) {
                    lo = Integer.parseInt(pm.group(1));
                    hi = pm.group(2) == null ? lo : Integer.parseInt(pm.group(2));
                }
                // Clips: any .wav whose name starts with this parent's stem.
                List<File> clips = new ArrayList<>();
                for (File g : entries) {
                    if (g.getName().endsWith(".wav") && g.getName().startsWith(stem)) {
                        clips.add(g);
                    }
                }
                Collections.sort(clips, (a, b) -> a.getName().compareTo(b.getName()));
                StringBuilder clipsJson = new StringBuilder("[");
                boolean firstClip = true;
                for (File c : clips) {
                    if (!firstClip) clipsJson.append(",");
                    firstClip = false;
                    String clipName = c.getName();
                    String clipDisplay = names.get(clipName);
                    Double clipDur = durations.get(clipName);
                    clipsJson.append("{\"name\":\"").append(clipName).append("\"")
                             .append(",\"size\":").append(c.length())
                             .append(",\"starred\":").append(starred.contains(clipName))
                             .append(",\"display_name\":")
                             .append(clipDisplay == null ? "null"
                                     : "\"" + escapeJsonString(clipDisplay) + "\"")
                             .append(",\"duration_sec\":")
                             .append(clipDur == null ? "null" : clipDur)
                             .append("}");
                }
                clipsJson.append("]");
                String pName = f.getName();
                String pDisplay = names.get(pName);
                Double pDur = durations.get(pName);
                json.append("{\"name\":\"").append(pName).append("\"")
                    .append(",\"size\":").append(f.length())
                    .append(",\"mtime\":").append(f.lastModified() / 1000.0)
                    .append(",\"has_sidecar\":").append(sidecar.exists())
                    .append(",\"bpm_lo\":").append(lo == null ? "null" : lo)
                    .append(",\"bpm_hi\":").append(hi == null ? "null" : hi)
                    .append(",\"starred\":").append(starred.contains(pName))
                    .append(",\"display_name\":")
                    .append(pDisplay == null ? "null"
                            : "\"" + escapeJsonString(pDisplay) + "\"")
                    .append(",\"duration_sec\":")
                    .append(pDur == null ? "null" : pDur)
                    .append(",\"clips\":").append(clipsJson)
                    .append("}");
            }
        }
        json.append("]");
        writeResponse(client, 200, "OK", "application/json",
                json.toString().getBytes(StandardCharsets.UTF_8));
    }

    private void serveRecording(Socket client, String urlName) throws IOException {
        String name = URLDecoder.decode(urlName, "UTF-8");
        // Defense against path traversal — we only serve direct children
        // of the metronome dir.
        if (name.contains("/") || name.contains("\\") || name.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        File f = new File(metronomeDir(), name);
        if (!f.exists() || !f.isFile()) {
            writeStatus(client, 404, "Not Found");
            return;
        }
        String ct = name.endsWith(".json") ? "application/json"
                  : name.endsWith(".m4a")  ? "audio/mp4"
                  : "application/octet-stream";
        OutputStream out = new BufferedOutputStream(client.getOutputStream());
        writeStatusLine(out, 200, "OK");
        writeHeader(out, "Content-Type", ct);
        writeHeader(out, "Content-Length", String.valueOf(f.length()));
        writeHeader(out, "Cache-Control", "no-cache");
        out.write("\r\n".getBytes(StandardCharsets.UTF_8));
        try (FileInputStream fin = new FileInputStream(f)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = fin.read(buf)) > 0) out.write(buf, 0, n);
        }
        out.flush();
    }

    // ---- POST /api/save-clip --------------------------------------------

    private void routePost(Socket client, String path, String query,
                           InputStream rawIn, BufferedReader reader,
                           int contentLength) throws IOException {
        if (path.equals("/api/package")) {
            makePackage(client, query);
            return;
        }
        if (path.equals("/api/delete")) {
            deleteRecording(client, query);
            return;
        }
        if (path.equals("/api/star")) {
            setStar(client, query);
            return;
        }
        if (path.equals("/api/markers")) {
            saveMarkers(client, query, rawIn, contentLength);
            return;
        }
        if (path.equals("/api/rename")) {
            setDisplayName(client, query);
            return;
        }
        if (path.equals("/api/duration")) {
            setDuration(client, query);
            return;
        }
        if (!path.equals("/api/save-clip")) {
            writeStatus(client, 404, "Not Found");
            return;
        }
        String name = paramFromQuery(query, "name");
        if (name == null || name.isEmpty() || name.contains("/")
                || name.contains("\\") || name.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        // Force .wav extension to keep things predictable.
        if (!name.endsWith(".wav")) name += ".wav";

        File out = new File(metronomeDir(), name);
        // BufferedReader has buffered some of the body already; we can't
        // easily switch back to InputStream. Read body via reader.
        // Since clip data is binary, this is wrong if we use a char reader.
        // Switch strategy: read directly from rawIn after the header,
        // using whatever the BufferedReader hasn't consumed via mark()/reset.
        // Simpler: skip the BufferedReader for body — we already consumed
        // up to the blank line; subsequent reads from rawIn give the body.
        try (FileOutputStream fos = new FileOutputStream(out)) {
            byte[] buf = new byte[64 * 1024];
            int remaining = contentLength;
            while (remaining > 0) {
                int n = rawIn.read(buf, 0, Math.min(buf.length, remaining));
                if (n < 0) break;
                fos.write(buf, 0, n);
                remaining -= n;
            }
        }
        String body = "{\"name\":\"" + name + "\",\"bytes\":" + out.length() + "}";
        writeResponse(client, 200, "OK", "application/json",
                body.getBytes(StandardCharsets.UTF_8));
    }

    private void deleteRecording(Socket client, String query) throws IOException {
        String name = paramFromQuery(query, "name");
        if (name == null || name.isEmpty() || name.contains("/")
                || name.contains("\\") || name.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        File dir = metronomeDir();
        java.util.List<String> removed = new ArrayList<>();
        if (name.endsWith(".m4a")) {
            String stem = name.substring(0, name.length() - 4);
            File m4a = new File(dir, name);
            File sidecar = new File(dir, stem + ".json");
            if (m4a.exists() && m4a.delete()) removed.add(m4a.getName());
            if (sidecar.exists() && sidecar.delete()) removed.add(sidecar.getName());
            File[] entries = dir.listFiles();
            if (entries != null) {
                for (File c : entries) {
                    if (c.getName().endsWith(".wav") && c.getName().startsWith(stem)) {
                        if (c.delete()) removed.add(c.getName());
                    }
                }
            }
        } else if (name.endsWith(".wav")) {
            File f = new File(dir, name);
            if (f.exists() && f.delete()) removed.add(f.getName());
        } else {
            writeStatus(client, 400, "must be .m4a or .wav");
            return;
        }
        Meta meta = loadMeta();
        meta.starred.removeAll(removed);
        for (String r : removed) {
            meta.names.remove(r);
            meta.durations.remove(r);
        }
        saveMeta(meta);
        StringBuilder b = new StringBuilder("{\"removed\":[");
        for (int i = 0; i < removed.size(); i++) {
            if (i > 0) b.append(",");
            b.append("\"").append(removed.get(i)).append("\"");
        }
        b.append("]}");
        writeResponse(client, 200, "OK", "application/json",
                b.toString().getBytes(StandardCharsets.UTF_8));
    }

    private void setStar(Socket client, String query) throws IOException {
        String name = paramFromQuery(query, "name");
        String value = paramFromQuery(query, "value");
        if (name == null || name.isEmpty() || name.contains("/")
                || name.contains("\\") || name.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        boolean on = !"false".equalsIgnoreCase(value);
        Meta meta = loadMeta();
        if (on) meta.starred.add(name); else meta.starred.remove(name);
        saveMeta(meta);
        String body = "{\"name\":\"" + name + "\",\"starred\":" + on + "}";
        writeResponse(client, 200, "OK", "application/json",
                body.getBytes(StandardCharsets.UTF_8));
    }

    // ---- /api/markers ---------------------------------------------------

    private File markersFile(String audioName) {
        // foo.m4a → foo.markers.json next to it (same convention as
        // tools/audio-editor/server.py — see _markers_path there).
        int dot = audioName.lastIndexOf('.');
        String stem = dot > 0 ? audioName.substring(0, dot) : audioName;
        return new File(metronomeDir(), stem + ".markers.json");
    }

    /**
     * Parse stored editor markers — `{"markers":[{"t_ms":N,"note":"..."},...]}`.
     * Hand-parsed to avoid pulling in a JSON library. Notes containing `}` or
     * escaped quotes are handled, but truly adversarial payloads aren't —
     * the only writer is our own POST endpoint.
     */
    private List<int[]> readEditorMarkers(String audioName, List<String> outNotes) {
        List<int[]> out = new ArrayList<>();
        File f = markersFile(audioName);
        if (!f.exists()) return out;
        try (java.io.FileReader fr = new java.io.FileReader(f)) {
            java.io.BufferedReader br = new java.io.BufferedReader(fr);
            StringBuilder sb = new StringBuilder();
            String line; while ((line = br.readLine()) != null) sb.append(line);
            Matcher mm = Pattern.compile(
                    "\\{[^{}]*?\"t_ms\"\\s*:\\s*(-?\\d+)[^{}]*?" +
                    "\"note\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"[^{}]*?\\}")
                .matcher(sb.toString());
            while (mm.find()) {
                int t = Integer.parseInt(mm.group(1));
                String note = unescapeJsonString(mm.group(2));
                out.add(new int[]{ t, 0 });
                outNotes.add(note);
            }
        } catch (Exception e) {
            Log.w(TAG, "readEditorMarkers failed", e);
        }
        return out;
    }

    /**
     * Pull `type=marker` events from the m4a's existing sidecar (written by
     * MetronomeService). These represent watch UP-button presses during
     * recording; the editor surfaces them as unannotated markers.
     */
    private List<Integer> readWatchMarkers(String audioName) {
        List<Integer> out = new ArrayList<>();
        if (!audioName.endsWith(".m4a")) return out;
        String stem = audioName.substring(0, audioName.length() - 4);
        File sidecar = new File(metronomeDir(), stem + ".json");
        if (!sidecar.exists()) return out;
        try (java.io.FileReader fr = new java.io.FileReader(sidecar)) {
            java.io.BufferedReader br = new java.io.BufferedReader(fr);
            StringBuilder sb = new StringBuilder();
            String line; while ((line = br.readLine()) != null) sb.append(line);
            Matcher mm = Pattern.compile(
                    "\\{[^{}]*?\"t_ms\"\\s*:\\s*(-?\\d+)[^{}]*?" +
                    "\"type\"\\s*:\\s*\"marker\"[^{}]*?\\}").matcher(sb.toString());
            while (mm.find()) out.add(Integer.parseInt(mm.group(1)));
        } catch (Exception e) {
            Log.w(TAG, "readWatchMarkers failed", e);
        }
        return out;
    }

    private void getMarkers(Socket client, String query) throws IOException {
        String audio = paramFromQuery(query, "audio");
        if (audio == null || audio.isEmpty() || audio.contains("/")
                || audio.contains("\\") || audio.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        List<String> notes = new ArrayList<>();
        List<int[]> editor = readEditorMarkers(audio, notes);
        // Dedupe by t_ms — editor's annotated markers win over the raw
        // sidecar event, mirroring the Python server's merge.
        java.util.HashSet<Integer> seen = new java.util.HashSet<>();
        for (int[] m : editor) seen.add(m[0]);
        List<Integer> watch = readWatchMarkers(audio);

        StringBuilder json = new StringBuilder("{\"markers\":[");
        boolean first = true;
        // Combine + sort by t_ms.
        List<int[]> combinedTs = new ArrayList<>();
        List<String> combinedNotes = new ArrayList<>();
        List<Boolean> combinedIsWatch = new ArrayList<>();
        for (int i = 0; i < editor.size(); i++) {
            combinedTs.add(editor.get(i));
            combinedNotes.add(notes.get(i));
            combinedIsWatch.add(false);
        }
        for (int t : watch) {
            if (seen.contains(t)) continue;
            combinedTs.add(new int[]{ t, 0 });
            combinedNotes.add("");
            combinedIsWatch.add(true);
        }
        // Insertion sort — marker counts are small (handful per recording).
        for (int i = 1; i < combinedTs.size(); i++) {
            for (int j = i; j > 0 && combinedTs.get(j)[0] < combinedTs.get(j - 1)[0]; j--) {
                int[] ts = combinedTs.get(j); combinedTs.set(j, combinedTs.get(j - 1)); combinedTs.set(j - 1, ts);
                String n = combinedNotes.get(j); combinedNotes.set(j, combinedNotes.get(j - 1)); combinedNotes.set(j - 1, n);
                Boolean w = combinedIsWatch.get(j); combinedIsWatch.set(j, combinedIsWatch.get(j - 1)); combinedIsWatch.set(j - 1, w);
            }
        }
        for (int i = 0; i < combinedTs.size(); i++) {
            if (!first) json.append(",");
            first = false;
            json.append("{\"t_ms\":").append(combinedTs.get(i)[0])
                .append(",\"note\":\"").append(escapeJsonString(combinedNotes.get(i))).append("\"");
            if (combinedIsWatch.get(i)) json.append(",\"source\":\"watch\"");
            json.append("}");
        }
        json.append("]}");
        writeResponse(client, 200, "OK", "application/json",
                json.toString().getBytes(StandardCharsets.UTF_8));
    }

    private void saveMarkers(Socket client, String query,
                             InputStream rawIn, int contentLength) throws IOException {
        String audio = paramFromQuery(query, "audio");
        if (audio == null || audio.isEmpty() || audio.contains("/")
                || audio.contains("\\") || audio.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        // Read JSON body off the raw stream (BufferedReader already consumed
        // the headers up to the blank line — see routePost comment).
        byte[] buf = new byte[contentLength];
        int got = 0;
        while (got < contentLength) {
            int n = rawIn.read(buf, got, contentLength - got);
            if (n < 0) break;
            got += n;
        }
        String body = new String(buf, 0, got, StandardCharsets.UTF_8);
        // Match individual marker objects inside the JSON array. Same
        // hand-parse pattern as readEditorMarkers.
        Matcher mm = Pattern.compile(
                "\\{[^{}]*?\"t_ms\"\\s*:\\s*(-?\\d+)[^{}]*?" +
                "\"note\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"[^{}]*?\\}").matcher(body);
        List<int[]> ts = new ArrayList<>();
        List<String> notes = new ArrayList<>();
        while (mm.find()) {
            ts.add(new int[]{ Integer.parseInt(mm.group(1)), 0 });
            String note = unescapeJsonString(mm.group(2));
            if (note.length() > 500) note = note.substring(0, 500);
            notes.add(note);
        }
        // Sort by t_ms before writing.
        for (int i = 1; i < ts.size(); i++) {
            for (int j = i; j > 0 && ts.get(j)[0] < ts.get(j - 1)[0]; j--) {
                int[] tt = ts.get(j); ts.set(j, ts.get(j - 1)); ts.set(j - 1, tt);
                String n = notes.get(j); notes.set(j, notes.get(j - 1)); notes.set(j - 1, n);
            }
        }
        StringBuilder out = new StringBuilder("{\n  \"markers\": [");
        for (int i = 0; i < ts.size(); i++) {
            if (i > 0) out.append(",");
            out.append("\n    {\"t_ms\": ").append(ts.get(i)[0])
               .append(", \"note\": \"").append(escapeJsonString(notes.get(i))).append("\"}");
        }
        out.append("\n  ]\n}\n");
        try (FileOutputStream fos = new FileOutputStream(markersFile(audio))) {
            fos.write(out.toString().getBytes(StandardCharsets.UTF_8));
        }
        String resp = "{\"count\":" + ts.size() + "}";
        writeResponse(client, 200, "OK", "application/json",
                resp.getBytes(StandardCharsets.UTF_8));
    }

    private static String escapeJsonString(String s) {
        StringBuilder b = new StringBuilder(s.length() + 4);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n");  break;
                case '\r': b.append("\\r");  break;
                case '\t': b.append("\\t");  break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }

    private static String unescapeJsonString(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                char n = s.charAt(++i);
                switch (n) {
                    case '"':  b.append('"');  break;
                    case '\\': b.append('\\'); break;
                    case 'n':  b.append('\n'); break;
                    case 'r':  b.append('\r'); break;
                    case 't':  b.append('\t'); break;
                    case '/':  b.append('/');  break;
                    default:   b.append(n);    break;
                }
            } else {
                b.append(c);
            }
        }
        return b.toString();
    }

    // ---- /api/rename + /api/duration -----------------------------------

    private void setDisplayName(Socket client, String query) throws IOException {
        String name = paramFromQuery(query, "name");
        String display = paramFromQuery(query, "display");
        if (name == null || name.isEmpty() || name.contains("/")
                || name.contains("\\") || name.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        Meta meta = loadMeta();
        if (display == null || display.trim().isEmpty()) {
            meta.names.remove(name);
        } else {
            String trimmed = display.trim();
            if (trimmed.length() > 200) trimmed = trimmed.substring(0, 200);
            meta.names.put(name, trimmed);
        }
        saveMeta(meta);
        String resp = "{\"name\":\"" + name + "\",\"display_name\":"
                + (meta.names.containsKey(name)
                    ? "\"" + escapeJsonString(meta.names.get(name)) + "\""
                    : "null")
                + "}";
        writeResponse(client, 200, "OK", "application/json",
                resp.getBytes(StandardCharsets.UTF_8));
    }

    private void setDuration(Socket client, String query) throws IOException {
        String name = paramFromQuery(query, "name");
        String secStr = paramFromQuery(query, "seconds");
        if (name == null || name.isEmpty() || secStr == null
                || name.contains("/") || name.contains("\\") || name.contains("..")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        double sec;
        try { sec = Double.parseDouble(secStr); }
        catch (NumberFormatException e) {
            writeStatus(client, 400, "bad seconds");
            return;
        }
        if (sec <= 0) {
            writeStatus(client, 400, "non-positive duration");
            return;
        }
        Meta meta = loadMeta();
        meta.durations.put(name, Math.round(sec * 1000.0) / 1000.0);
        saveMeta(meta);
        String resp = "{\"name\":\"" + name + "\",\"duration_sec\":"
                + meta.durations.get(name) + "}";
        writeResponse(client, 200, "OK", "application/json",
                resp.getBytes(StandardCharsets.UTF_8));
    }

    private void makePackage(Socket client, String query) throws IOException {
        String name = paramFromQuery(query, "name");
        if (name == null || name.isEmpty() || name.contains("/")
                || name.contains("\\") || name.contains("..")
                || !name.endsWith(".m4a")) {
            writeStatus(client, 400, "Bad Request");
            return;
        }
        File dir = metronomeDir();
        File m4a = new File(dir, name);
        if (!m4a.exists()) {
            writeStatus(client, 404, "Not Found");
            return;
        }
        String stem = name.substring(0, name.length() - 4);
        File sidecar = new File(dir, stem + ".json");
        // Save next to the m4a so the file's name alone is enough for the
        // ContentProvider / share bridge to find it. The file-list endpoint
        // filters to *.m4a so the zip doesn't pollute the recordings UI.
        File zipOut = new File(dir, stem + ".zip");
        try (java.util.zip.ZipOutputStream zos = new java.util.zip.ZipOutputStream(
                new java.io.BufferedOutputStream(new FileOutputStream(zipOut)))) {
            zipAdd(zos, m4a);
            if (sidecar.exists()) zipAdd(zos, sidecar);
        }
        String body = "{\"name\":\"" + zipOut.getName() + "\","
                + "\"path\":\"" + zipOut.getAbsolutePath() + "\","
                + "\"bytes\":" + zipOut.length() + "}";
        writeResponse(client, 200, "OK", "application/json",
                body.getBytes(StandardCharsets.UTF_8));
    }

    private static void zipAdd(java.util.zip.ZipOutputStream zos, File f) throws IOException {
        zos.putNextEntry(new java.util.zip.ZipEntry(f.getName()));
        try (FileInputStream fin = new FileInputStream(f)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = fin.read(buf)) > 0) zos.write(buf, 0, n);
        }
        zos.closeEntry();
    }

    private String paramFromQuery(String query, String key) {
        if (query == null || query.isEmpty()) return null;
        for (String kv : query.split("&")) {
            int eq = kv.indexOf('=');
            if (eq <= 0) continue;
            if (kv.substring(0, eq).equals(key)) {
                try { return URLDecoder.decode(kv.substring(eq + 1), "UTF-8"); }
                catch (Exception e) { return null; }
            }
        }
        return null;
    }

    // ---- HTTP helpers ---------------------------------------------------

    private static byte[] readAll(InputStream in) throws IOException {
        byte[] buf = new byte[8192];
        java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream();
        int n;
        while ((n = in.read(buf)) > 0) baos.write(buf, 0, n);
        return baos.toByteArray();
    }

    private static void writeResponse(Socket client, int code, String msg,
                                      String contentType, byte[] body) throws IOException {
        OutputStream out = client.getOutputStream();
        writeStatusLine(out, code, msg);
        writeHeader(out, "Content-Type", contentType);
        writeHeader(out, "Content-Length", String.valueOf(body.length));
        writeHeader(out, "Cache-Control", "no-cache");
        out.write("\r\n".getBytes(StandardCharsets.UTF_8));
        out.write(body);
        out.flush();
    }

    private static void writeStatus(Socket client, int code, String msg) throws IOException {
        byte[] body = (code + " " + msg).getBytes(StandardCharsets.UTF_8);
        writeResponse(client, code, msg, "text/plain; charset=utf-8", body);
    }

    private static void writeStatusLine(OutputStream out, int code, String msg) throws IOException {
        out.write(("HTTP/1.1 " + code + " " + msg + "\r\n").getBytes(StandardCharsets.UTF_8));
    }

    private static void writeHeader(OutputStream out, String name, String value) throws IOException {
        out.write((name + ": " + value + "\r\n").getBytes(StandardCharsets.UTF_8));
    }
}
