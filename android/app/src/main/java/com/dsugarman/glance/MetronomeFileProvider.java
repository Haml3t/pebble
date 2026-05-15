package com.dsugarman.glance;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

/**
 * Custom ContentProvider exposing files in the metronome recordings dir
 * (m4a + wav + json + zip) as {@code content://com.dsugarman.glance.files/<name>}
 * URIs so they can be passed via {@code Intent.ACTION_SEND} to share sheets
 * and other apps without triggering FileUriExposedException on Android 7+.
 *
 * Why custom instead of androidx FileProvider: the project has one Android
 * dependency (PebbleKit) and the user's existing builds run cleanly without
 * the AndroidX migration. A 60-line provider does what we need.
 */
public class MetronomeFileProvider extends ContentProvider {

    public static final String AUTHORITY = "com.dsugarman.glance.files";

    public static Uri uriFor(String name) {
        return Uri.parse("content://" + AUTHORITY + "/" + Uri.encode(name));
    }

    @Override public boolean onCreate() { return true; }

    private File resolveFile(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null) return null;
        // Hard reject path-traversal attempts. Allowed file names are flat —
        // no slashes, no .. — which keeps every served file inside the
        // metronome dir.
        if (name.contains("/") || name.contains("\\") || name.contains("..")) return null;
        File dir = new File(
                getContext().getExternalFilesDir(Environment.DIRECTORY_MUSIC),
                "metronome");
        return new File(dir, name);
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File f = resolveFile(uri);
        if (f == null || !f.exists() || !f.isFile()) {
            throw new FileNotFoundException(uri.toString());
        }
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        String n = uri.getLastPathSegment();
        if (n == null) return null;
        if (n.endsWith(".m4a"))  return "audio/mp4";
        if (n.endsWith(".wav"))  return "audio/wav";
        if (n.endsWith(".zip"))  return "application/zip";
        if (n.endsWith(".json")) return "application/json";
        return "application/octet-stream";
    }

    /** Share sheets read display name + size from this query — without it
     *  Gmail attaches as "Unknown file" with zero bytes. */
    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        File f = resolveFile(uri);
        if (f == null || !f.exists()) return null;
        if (projection == null) {
            projection = new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        }
        MatrixCursor c = new MatrixCursor(projection);
        Object[] row = new Object[projection.length];
        for (int i = 0; i < projection.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(projection[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(projection[i]))     row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    // Read-only; no insert/update/delete via the provider — the editor
    // server handles mutations via its own /api/* endpoints.
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String s, String[] sa) { return 0; }
    @Override public int update(Uri uri, ContentValues v, String s, String[] sa) { return 0; }
}
