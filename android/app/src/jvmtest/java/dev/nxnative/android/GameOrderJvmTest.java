package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Home's tile order (GameOrder) on the JVM; prints "ok &lt;n&gt;" and exits 0, or the first failure and exits 1. Run by dm/tools/tests/test_android_gameorder.py */
public final class GameOrderJvmTest {
    private static int n;

    private static void eq(Object want, Object got, String what) {
        n++;
        if (!java.util.Objects.equals(want, got)) { System.out.println("FAIL " + what + ": want " + want + ", got " + got); System.exit(1); }
    }

    private static List<String> sort(List<String> games, List<String> order) { return GameOrder.sort(games, order, g -> g); }

    public static void main(String[] a) {
        String A = GameOrder.id("0100000000aa0000", "/p/a"), B = GameOrder.id("0100000000AB0000", "/p/b"), C = GameOrder.id("0100000000AC0000", "/p/c");
        eq("0100000000AA0000|/p/a", A, "id upper-cases the title ID");
        // no stored order: the list as it is
        eq(Arrays.asList(A, B, C), sort(Arrays.asList(A, B, C), new ArrayList<>()), "empty order");
        // the order wins over the list's position (ids, not positions)
        eq(Arrays.asList(C, A, B), sort(Arrays.asList(A, B, C), Arrays.asList(C, A, B)), "stored order");
        eq(Arrays.asList(C, A, B), sort(Arrays.asList(B, C, A), Arrays.asList(C, A, B)), "same order from another list order");
        // a new game goes last, in its current order
        String D = GameOrder.id("0100000000AD0000", "/p/d"), E = GameOrder.id("0100000000AE0000", "/p/e");
        eq(Arrays.asList(C, A, B, D, E), sort(Arrays.asList(D, A, E, B, C), Arrays.asList(C, A, B)), "new games last");
        // a removed game: its id is ignored, and the next write leaves it out
        List<String> shown = sort(Arrays.asList(A, C), Arrays.asList(C, B, A));
        eq(Arrays.asList(C, A), shown, "removed game ignored");
        eq(Arrays.asList(C, A), GameOrder.write(shown), "removed game dropped on write");
        // stable on repeat
        List<String> once = sort(Arrays.asList(D, A, E, B, C), Arrays.asList(C, A, B));
        eq(once, sort(once, GameOrder.write(once)), "sorting again changes nothing");
        // move: one place, clamped, unknown id
        eq(Arrays.asList(B, A, C), GameOrder.move(Arrays.asList(A, B, C), A, 1), "move right");
        eq(Arrays.asList(A, C, B), GameOrder.move(Arrays.asList(A, B, C), C, -1), "move left");
        eq(Arrays.asList(A, B, C), GameOrder.move(Arrays.asList(A, B, C), A, -1), "left at the start");
        eq(Arrays.asList(A, B, C), GameOrder.move(Arrays.asList(A, B, C), C, 1), "right at the end");
        eq(Arrays.asList(A, B, C), GameOrder.move(Arrays.asList(A, B, C), "x|y", 1), "unknown id");
        // a merged entry (same title, another pack) takes its old place
        String A2 = GameOrder.id("0100000000AA0000", "/p/a2");
        eq(Arrays.asList(C, A2, B), sort(Arrays.asList(A2, B, C), Arrays.asList(C, A, B)), "merged entry keeps the place");
        // two entries of one title (both listed) keep their own places
        eq(Arrays.asList(A2, B, A), sort(Arrays.asList(A, B, A2), Arrays.asList(A2, B, A)), "two dumps of one title");
        // a recency list: the game played comes first, the others as shown
        eq(Arrays.asList(B, C, A), GameOrder.front(Arrays.asList(C, A, B), B), "played game first");
        eq(Arrays.asList(C, A, B), GameOrder.front(Arrays.asList(C, A, B), C), "first stays first");
        // the first time (no order kept): played games by their last play, newest first, then the rest in list order
        java.util.Map<String, Long> used = new java.util.HashMap<>();
        used.put(B, 100L); used.put(D, 300L);
        eq(Arrays.asList(D, B, A, C, E), GameOrder.recent(new ArrayList<>(), Arrays.asList(A, B, C, D, E), id -> used.getOrDefault(id, 0L)), "first time: by last play");
        // games Home has not had before go first, the newest (last added) first; known titles do not move
        eq(Arrays.asList(E, D, C, A, B), GameOrder.recent(Arrays.asList(C, A, B), Arrays.asList(A, B, C, D, E), id -> 0L), "new games first, newest first");
        eq(null, GameOrder.recent(Arrays.asList(C, A, B), Arrays.asList(A, B, C), id -> 0L), "nothing new: unchanged");
        eq(null, GameOrder.recent(Arrays.asList(C, A, B), Arrays.asList(A2, B, C), id -> 0L), "a prepared entry (same title, another pack) is not new");
        eq(Arrays.asList(C, A2, B), sort(Arrays.asList(A2, B, C), Arrays.asList(C, A, B)), "and it keeps its place");
        // the stored form
        List<String> ids = Arrays.asList(A, B, C);
        eq(ids, GameOrder.decode(GameOrder.encode(ids)), "encode and decode");
        eq(0, GameOrder.decode(null).size() + GameOrder.decode("").size(), "empty decode");
        System.out.println("ok " + n);
    }
}
