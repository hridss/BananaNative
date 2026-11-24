package dev.nxnative.android;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The album's rules without Android (AlbumExport, dm/tools/tests/test_android_album.py): which files in a game's album folder (DM_ALBUM_DIR, where the host's
 * hle_album.c publishes each nn::album::SaveScreenshot picture as a PNG) are finished pictures, and the gallery folder they are copied into.
 */
final class AlbumFiles {
    private AlbumFiles() {}

    /** A finished picture: a PNG the host published (never its hidden temporary file, .album-XXXXXX.tmp) */
    static boolean exportable(String name) {
        return name != null && !name.startsWith(".") && name.length() > 4 && name.regionMatches(true, name.length() - 4, ".png", 0, 4);
    }

    /** The finished pictures in a game's album folder, oldest name first (the names start with the time they were taken) */
    static List<File> pending(File dir) {
        File[] l = dir == null ? null : dir.listFiles();
        List<File> out = new ArrayList<>();
        if (l == null) return out;
        Arrays.sort(l);
        for (File f : l) if (f.isFile() && exportable(f.getName())) out.add(f);
        return out;
    }

    /** One folder name from a label: no path separators or characters other apps' file systems refuse, no leading dots, at most 64 characters; fallback when nothing is left */
    static String folderName(String label, String fallback) {
        StringBuilder b = new StringBuilder();
        String s = label == null ? "" : label.trim();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            b.append(c < 0x20 || c == 0x7f || "/\\:*?\"<>|".indexOf(c) >= 0 ? '_' : c);
        }
        String r = b.toString().replaceAll("^[.\\s]+", "").trim();
        if (r.length() > 64) r = r.substring(0, 64).trim();
        return r.isEmpty() ? fallback : r;
    }

    /** MediaStore's RELATIVE_PATH for a game's pictures: Pictures/<app name>/<game name> (the title ID when the name gives no folder name) */
    static String relativePath(String appName, String gameName, String titleId) {
        return "Pictures/" + folderName(appName, "Banana Native") + "/" + folderName(gameName, folderName(titleId, "Game")) + "/";
    }
}
