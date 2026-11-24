package dev.nxnative.android;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * A program switch (android/README.md, "Programs of one game"; docs/reference/services.md, "Multi-program applications: oe::ExecuteProgram"). A game of several programs
 * (Super Mario 3D All-Stars: the launcher, Super Mario 64, Sunshine) runs one program per :game process, as the host is one program per run. When the game calls
 * oe::ExecuteProgram (or oe::RequestToRelaunchApplication), the host writes the next program's index and the launch parameters to DM_EXEC_OUT ({@link #record}: a
 * little-endian int, then the parameters) and calls nxhost_program_callback, which reaches {@link #handoff} through nxlaunch.c before the host's graceful exit.
 * <p>
 * The game's screen (GameActivity.programHandoff) writes the request into the app's own files ({@link #request}: what to start, the parameters in a file of their own,
 * the process that ends), starts Home with {@link #EXTRA} and finishes; the host's exit ends the :game process. Home ({@link #take}) waits until that process is gone
 * (its save lock, its host), then starts the game's screen again: the same game, pack and save, with "program" (the pack's p&lt;n&gt;/ folder and DM_PROGRAM),
 * "launch_param" (DM_LAUNCH_PARAM), "prev_program" (DM_PREV_PROGRAM) or "relaunch" (DM_RELAUNCH=1), and the host's note ("note": why the program that ended could not
 * run, shown to the player). The request is read from the app's files, never from the intent, so another app cannot start a game with it; one older than a minute is
 * dropped.
 */
final class ProgramSwitch {
    private ProgramSwitch() {}

    static final String EXTRA = "program_switch";
    private static final String TAG = "nxnative";
    private static final long MAX_AGE_MS = 60_000, WAIT_MS = 8_000;

    /** DM_EXEC_OUT of a game: the host's record of the next program */
    static File record(Context c, String titleId) { return new File(c.getFilesDir(), "program-exec-" + titleId + ".bin"); }
    /** The launch parameters the next program pops (DM_LAUNCH_PARAM) */
    static File param(Context c, String titleId) { return new File(c.getFilesDir(), "program-param-" + titleId + ".bin"); }
    /** The request the game's screen leaves for Home */
    static File request(Context c) { return new File(c.getFilesDir(), "program-switch.tsv"); }

    // ---- The game's process ----
    /** The game's screen that started the host (GameActivity.start): the one a switch goes through */
    static volatile GameActivity screen;

    /** nxlaunch.c, on a thread attached for the call while the guest thread waits: the screen does its part on the UI thread, and the host ends when this returns */
    static void handoff(int index, int prev, byte[] note) {
        GameActivity a = screen;
        if (a == null) { Log.w(TAG, "program switch to " + index + ": no game screen; the game ends"); return; }
        CountDownLatch done = new CountDownLatch(1);
        String n = note == null ? null : new String(note, StandardCharsets.UTF_8);
        new Handler(Looper.getMainLooper()).post(() -> {
            try { a.programHandoff(index, prev, n); }
            catch (RuntimeException e) { Log.w(TAG, "program switch: " + e); }
            finally { done.countDown(); }
        });
        try { done.await(5, TimeUnit.SECONDS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }

    /** The launch parameters of DM_EXEC_OUT's record: the bytes after the index when it names index, else none (a short or foreign record) */
    static byte[] params(byte[] rec, int index) {
        if (rec == null || rec.length < 4) return new byte[0];
        int ix = (rec[0] & 0xff) | (rec[1] & 0xff) << 8 | (rec[2] & 0xff) << 16 | (rec[3] & 0xff) << 24;
        return ix == index ? Arrays.copyOfRange(rec, 4, rec.length) : new byte[0];
    }

    /** The request's text: key, tab, value lines (values on one line) */
    static String format(Map<String, String> req) {
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : req.entrySet())
            b.append(e.getKey()).append('\t').append(String.valueOf(e.getValue()).replace('\n', ' ').replace('\t', ' ')).append('\n');
        return b.toString();
    }

    static Map<String, String> parse(String text) {
        Map<String, String> m = new LinkedHashMap<>();
        if (text == null) return m;
        for (String l : text.split("\n")) {
            int t = l.indexOf('\t');
            if (t > 0) m.put(l.substring(0, t), l.substring(t + 1));
        }
        return m;
    }

    /** Whether a request may be taken: it names a game and its pack, a program 0 to 15, and is at most a minute old at now */
    static boolean valid(Map<String, String> req, long now) {
        long t;
        int p;
        try { t = Long.parseLong(req.getOrDefault("time", "")); p = Integer.parseInt(req.getOrDefault("program", "")); }
        catch (NumberFormatException e) { return false; }
        return p >= 0 && p <= 15 && now - t >= 0 && now - t <= MAX_AGE_MS && !req.getOrDefault("title_id", "").isEmpty()
                && !req.getOrDefault("game", "").isEmpty() && !req.getOrDefault("pack", "").isEmpty();
    }

    /** The game's screen for a request: the game, its pack and the program's start (GameActivity reads these extras) */
    static Intent gameIntent(Context c, Map<String, String> req) {
        Intent i = new Intent(c, GameActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK)
                .putExtra("title_id", req.get("title_id")).putExtra("name", req.getOrDefault("name", "")).putExtra("game", req.get("game")).putExtra("pack", req.get("pack"))
                .putExtra("program", Integer.parseInt(req.get("program")));
        if ("1".equals(req.get("frontend"))) i.putExtra("frontend", true);
        if ("1".equals(req.get("relaunch"))) i.putExtra("relaunch", true);
        else i.putExtra("prev_program", Integer.parseInt(req.getOrDefault("prev", "-1")));
        String lp = req.getOrDefault("param", "");
        if (!lp.isEmpty()) i.putExtra("launch_param", lp);
        String note = req.getOrDefault("note", "");
        if (!note.isEmpty()) i.putExtra("note", note);
        return i;
    }

    // ---- Home ----
    /** MainActivity.onCreate / onNewIntent: a switch Home was started for. True when the intent was one (the game starts once the old process is gone) */
    static boolean take(Activity home, Intent launch) {
        if (launch == null || !launch.getBooleanExtra(EXTRA, false)) return false;
        launch.removeExtra(EXTRA);
        File f = request(home);
        Map<String, String> req;
        try { req = parse(f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8) : null); }
        catch (IOException e) { req = parse(null); }
        if (!f.delete() && f.exists()) Log.w(TAG, "program switch: " + f + " could not be removed");
        if (!valid(req, System.currentTimeMillis())) { Log.w(TAG, "program switch: no current request in " + f + "; nothing started"); return true; }
        String note = req.getOrDefault("note", "");
        if (!note.isEmpty()) Toast.makeText(home, note, Toast.LENGTH_LONG).show();
        int pid;
        try { pid = Integer.parseInt(req.getOrDefault("pid", "0")); } catch (NumberFormatException e) { pid = 0; }
        final Map<String, String> r = req;
        final int old = pid;
        final long until = System.currentTimeMillis() + WAIT_MS;
        Handler ui = new Handler(Looper.getMainLooper());
        ui.post(new Runnable() {
            @Override public void run() {
                if (home.isFinishing() || home.isDestroyed()) return;
                if (old > 0 && ExitWatch.alive(home, old) && System.currentTimeMillis() < until) { ui.postDelayed(this, 100); return; }
                if (old > 0 && ExitWatch.alive(home, old)) { Log.w(TAG, "program switch: the game's process " + old + " is still running after " + WAIT_MS / 1000 + " s; nothing started"); return; }
                GameReturn.remember(home, r.get("title_id"), r.get("name"), r.get("game"), r.get("pack"));
                Log.i(TAG, "program switch: " + r.get("title_id") + " program " + r.get("program") + " starts");
                home.startActivity(gameIntent(home, r));
            }
        });
        return true;
    }
}
