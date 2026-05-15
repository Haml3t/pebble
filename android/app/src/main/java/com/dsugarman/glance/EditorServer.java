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

    private java.util.HashSet<String> loadStarred() {
        java.util.HashSet<String> set = new java.util.HashSet<>();
        File f = metaFile();
        if (!f.exists()) return set;
        try (java.io.FileReader fr = new java.io.FileReader(f)) {
            java.io.BufferedReader br = new java.io.BufferedReader(fr);
            StringBuilder sb = new StringBuilder();
            String line; while ((line = br.readLine()) != null) sb.append(line);
            String body = sb.toString();
            // Tiny hand-parse — the file only ever contains
            // {"starred":["foo","bar",...]}. Avoids pulling in org.json.
            Matcher mm = Pattern.compile("\"starred\"\\s*:\\s*\\[(.*?)\\]",
                    Pattern.DOTALL).matcher(body);
            if (mm.find()) {
                String inside = mm.group(1);
                Matcher nm = Pattern.compile("\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(inside);
                while (nm.find()) set.add(nm.group(1));
            }
        } catch (IOException ignored) {}
        return set;
    }

    private void saveStarred(java.util.Set<String> starred) {
        StringBuilder sb = new StringBuilder();
        sb.append("{\n  \"starred\": [");
        boolean first = true;
        java.util.List<String> sorted = new ArrayList<>(starred);
        java.util.Collections.sort(sorted);
        for (String s : sorted) {
            if (!first) sb.append(",");
            first = false;
            sb.append("\n    \"").append(s.replace("\\", "\\\\").replace("\"", "\\\"")).append("\"");
        }
        sb.append("\n  ]\n}\n");
        try (FileOutputStream fos = new FileOutputStream(metaFile())) {
            fos.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "saveStarred failed", e);
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
                routeGet(client, path);
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

    private void routeGet(Socket client, String path) throws IOException {
        if (path.equals("/") || path.equals("/index.html")) {
            serveAsset(client, "audio-editor/index.html", "text/html; charset=utf-8");
        } else if (path.equals("/app.js")) {
            serveAsset(client, "audio-editor/app.js", "application/javascript; charset=utf-8");
        } else if (path.equals("/style.css")) {
            serveAsset(client, "audio-editor/style.css", "text/css; charset=utf-8");
        } else if (path.equals("/api/files")) {
            serveFileList(client);
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
        java.util.HashSet<String> starred = loadStarred();
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
                    clipsJson.append("{\"name\":\"").append(c.getName()).append("\"")
                             .append(",\"size\":").append(c.length())
                             .append(",\"starred\":").append(starred.contains(c.getName()))
                             .append("}");
                }
                clipsJson.append("]");
                json.append("{\"name\":\"").append(f.getName()).append("\"")
                    .append(",\"size\":").append(f.length())
                    .append(",\"mtime\":").append(f.lastModified() / 1000.0)
                    .append(",\"has_sidecar\":").append(sidecar.exists())
                    .append(",\"bpm_lo\":").append(lo == null ? "null" : lo)
                    .append(",\"bpm_hi\":").append(hi == null ? "null" : hi)
                    .append(",\"starred\":").append(starred.contains(f.getName()))
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
        java.util.HashSet<String> starred = loadStarred();
        starred.removeAll(removed);
        saveStarred(starred);
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
        java.util.HashSet<String> starred = loadStarred();
        if (on) starred.add(name); else starred.remove(name);
        saveStarred(starred);
        String body = "{\"name\":\"" + name + "\",\"starred\":" + on + "}";
        writeResponse(client, 200, "OK", "application/json",
                body.getBytes(StandardCharsets.UTF_8));
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
