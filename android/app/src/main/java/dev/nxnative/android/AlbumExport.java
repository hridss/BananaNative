package dev.nxnative.android;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.FileObserver;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Pictures a game saves to the console album reach the phone's gallery. The host's hle_album.c publishes each nn::album::SaveScreenshot picture as a PNG in
 * DM_ALBUM_DIR, which GameActivity sets to {@link #dir} (the app's external files, album/&lt;title ID&gt;); this copies every finished PNG there into MediaStore
 * (Pictures/&lt;app name&gt;/&lt;game name&gt;, AlbumFiles.relativePath) and removes the copied file. A file appears whole (a hard link or a rename of the host's
 * temporary file), so a FileObserver on the folder sees it at once; the folder is also swept when the watch starts, for pictures a run left behind (a copy that
 * failed, or a process that ended first). Inserting into the app's own MediaStore rows needs no permission on Android 10 and later (the APK's minSdk is 31).
 * One watch per process: the host runs once per :game process and keeps running when its screen is recreated.
 */
final class AlbumExport {
    private AlbumExport() {}

    private static FileObserver observer;   // held here: an observer that is collected stops watching
    private static final ExecutorService WORK = Executors.newSingleThreadExecutor(r -> { Thread t = new Thread(r, "nx-album"); t.setDaemon(true); return t; });
    private static final Set<String> QUEUED = new HashSet<>();

    /** The game's album folder (DM_ALBUM_DIR); the host creates it at its first picture */
    static File dir(Context c, String titleId) { return new File(c.getExternalFilesDir(null), "album/" + titleId); }

    /** Starts watching the game's album folder (once per process) and copies what is already there */
    static synchronized void watch(Context context, File dir, String gameName, String titleId) {
        if (observer != null) return;
        Context app = context.getApplicationContext();
        String rel = AlbumFiles.relativePath(app.getString(R.string.app_name), gameName, titleId);
        if (!dir.isDirectory() && !dir.mkdirs()) { Native.note("[RED] album: cannot create " + dir + ": pictures the game saves to the album stay out of the gallery"); return; }
        observer = new FileObserver(dir, FileObserver.CREATE | FileObserver.MOVED_TO) {
            @Override public void onEvent(int event, String name) {
                if (AlbumFiles.exportable(name)) queue(app, new File(dir, name), rel);
            }
        };
        observer.startWatching();
        for (File f : AlbumFiles.pending(dir)) queue(app, f, rel);
    }

    private static void queue(Context app, File f, String rel) {
        synchronized (QUEUED) { if (!QUEUED.add(f.getName())) return; }
        WORK.execute(() -> {
            try {
                if (!f.isFile()) return;   // copied already (an event and the sweep both saw it)
                String err = copy(app, f, rel);
                if (err != null) Native.note("[RED] album: " + f.getName() + " not copied to the gallery (" + err + "); it stays in " + f.getParent() + " and is tried again at the next start");
                else if (!f.delete()) Native.note("[nxlaunch] album: " + f.getName() + " copied to " + rel + " but not removed from " + f.getParent());
                else Native.note("[nxlaunch] album: " + f.getName() + " copied to the gallery, " + rel);
            } finally {
                synchronized (QUEUED) { QUEUED.remove(f.getName()); }
            }
        });
    }

    /** One PNG into MediaStore as a pending row, written, then published; null on success, else what failed (the row is removed) */
    private static String copy(Context app, File f, String rel) {
        ContentResolver cr = app.getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, f.getName());
        v.put(MediaStore.MediaColumns.MIME_TYPE, "image/png");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, rel);
        v.put(MediaStore.Images.ImageColumns.DATE_TAKEN, f.lastModified());
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri;
        try { uri = cr.insert(MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), v); }
        catch (RuntimeException e) { return "MediaStore refused it: " + e.getMessage(); }
        if (uri == null) return "MediaStore refused it";
        try (InputStream in = new FileInputStream(f); OutputStream out = cr.openOutputStream(uri)) {
            if (out == null) throw new IOException("no output stream");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        } catch (IOException | RuntimeException e) {
            try { cr.delete(uri, null, null); } catch (RuntimeException ignored) { /* the pending row expires on its own */ }
            return String.valueOf(e.getMessage());
        }
        v.clear();
        v.put(MediaStore.MediaColumns.IS_PENDING, 0);
        try { cr.update(uri, v, null, null); }
        catch (RuntimeException e) { try { cr.delete(uri, null, null); } catch (RuntimeException ignored) { /* as above */ } return "not published: " + e.getMessage(); }
        return null;
    }
}
