package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.List;

/** AlbumFiles on the JVM (dm/tools/tests/test_android_album.py): which files of the album folder are finished pictures, and the gallery folder names. Prints "ok" or throws */
public final class AlbumFilesJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }

    public static void main(String[] args) throws IOException {
        check(AlbumFiles.exportable("20261124_105708_0400A3A01EE4C000.png"), "a published picture");
        check(AlbumFiles.exportable("20261124_105708_0400A3A01EE4C000_01.PNG"), "the extension in any case");
        check(!AlbumFiles.exportable(".album-Ab12Cd.tmp"), "the host's temporary file");
        check(!AlbumFiles.exportable(".hidden.png"), "a hidden file");
        check(!AlbumFiles.exportable("notes.txt") && !AlbumFiles.exportable(".png") && !AlbumFiles.exportable(null), "other names");

        check(AlbumFiles.relativePath("Banana Native", "Pokémon Pokopia", "0400A3A01EE4C000").equals("Pictures/Banana Native/Pokémon Pokopia/"), "plain names");
        check(AlbumFiles.relativePath("Banana Native test", "A/B: C?", "0400A3A01EE4C000").equals("Pictures/Banana Native test/A_B_ C_/"), "separators and refused characters");
        check(AlbumFiles.relativePath("Banana Native", "  ..  ", "0400A3A01EE4C000").equals("Pictures/Banana Native/0400A3A01EE4C000/"), "nothing left: the title ID");
        check(AlbumFiles.relativePath("", null, null).equals("Pictures/Banana Native/Game/"), "no names at all");
        check(AlbumFiles.folderName("x".repeat(100), "f").length() == 64, "at most 64 characters");
        check(AlbumFiles.folderName("a\u0001b", "f").equals("a_b"), "control characters");

        File dir = Files.createTempDirectory("album").toFile();
        try {
            for (String n : new String[] {"20261124_120000_X.png", ".album-1.tmp", "20261124_110000_X.png", "readme.txt"})
                Files.write(new File(dir, n).toPath(), new byte[] {1});
            check(new File(dir, "sub.png").mkdir(), "a folder named like a picture");
            List<File> l = AlbumFiles.pending(dir);
            check(l.size() == 2 && l.get(0).getName().equals("20261124_110000_X.png") && l.get(1).getName().equals("20261124_120000_X.png"), "pending: pictures only, oldest first: " + l);
            check(AlbumFiles.pending(new File(dir, "missing")).isEmpty() && AlbumFiles.pending(null).isEmpty(), "no folder: nothing pending");
        } finally {
            File[] l = dir.listFiles();
            if (l != null) for (File f : l) if (!f.delete()) throw new IOException("cannot remove " + f);
            if (!dir.delete()) throw new IOException("cannot remove " + dir);
        }
        System.out.println("ok");
    }
}
