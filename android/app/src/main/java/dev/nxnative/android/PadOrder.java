package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Which gamepad is player 1 (GameActivity's input devices; pure Java, so PadOrderJvmTest checks it on the JVM). Android reports more than controllers as gamepads:
 * a phone's own input devices (Xiaomi and POCO phones have "uinput-xiaomi", which is neither virtual nor silent about gamepad sources) took player 1 by connecting
 * first, and a game that reads only player 1 ignored the real controller on player 2 (issues #42, #62). What Android reports decides, never the device's name:
 * <ul>
 * <li>A device's rank (rank()): the device chosen as player 1 (the in-game panel's Player 1, by its descriptor); then an external device (USB, Bluetooth:
 *     InputDevice.isExternal) with gamepad buttons and a stick; another external gamepad; a built-in one with gamepad buttons, a stick and a vendor ID; the rest.</li>
 * <li>External devices and the chosen one get a player when they connect, best rank first (at the start the order of ids within a rank). The others get one only
 *     from their first input (a gamepad button, or a stick or trigger pushed past half: wakes()), so a device that never sends any takes no player and does not hide
 *     the on-screen controls. A built-in controller (a handheld's) gets its player from its first press.</li>
 * <li>A device that gets a player takes player 1 when player 1 is free or held by a device of a worse rank (Host.first: they trade players); one that gets it by
 *     its input also when player 1's device is of the same rank and no controller (BUILT_IN, OTHER): the device the player uses wins over one that woke by itself.</li>
 * <li>A left and a right half of one controller (half(): a Joy-Con L and a Joy-Con R, Android reports each as a device of its own) play as one controller: they take
 *     one player together (Host.pair, pad_pair.h), and the host puts the left half's stick on the left stick and the right half's on the right one. With that setting
 *     off (Prefs.pairJoycons) each half is a controller of its own, for two players with one Joy-Con each.</li>
 * </ul>
 */
final class PadOrder {
    /** What Android reports of an input device (InputDevice): its id, descriptor (stable across reconnects and restarts), name, isExternal, gamepad buttons plus X / Y axes,
     *  vendor ID, and which half of a split controller it is (-1 left, 1 right, 0 a whole pad) */
    static final class Pad {
        final int id; final String key, name; final boolean external, full; final int vendor, half;
        Pad(int id, String key, String name, boolean external, boolean full, int vendor, int half) {
            this.id = id; this.key = key == null ? "" : key; this.name = name == null ? "" : name; this.external = external; this.full = full; this.vendor = vendor;
            this.half = half;
        }
    }

    /** A left and a right half (Joy-Con L + R, half()) */
    static final int HALF_LEFT = -1, HALF_RIGHT = 1;

    /** The host's players (Native.deviceAdded / deviceFirst / devicePlayer / devicePair) */
    interface Host {
        int add(Pad p);     // the player it gets (0..7), -1 with every player taken
        int first(Pad p);   // it becomes player 1 (player 1's device takes its old player): 0, or -1
        int player(int id); // -1: none
        int pair(Pad left, Pad right);   // the two halves as one controller: their player, -1 when none yet, -2 when the host cannot pair them
    }

    static final int CHOSEN = 0, CONTROLLER = 1, EXTERNAL = 2, BUILT_IN = 3, OTHER = 4;

    static int rank(Pad p, String chosen) {
        if (chosen != null && !chosen.isEmpty() && chosen.equals(p.key)) return CHOSEN;
        if (p.external) return p.full ? CONTROLLER : EXTERNAL;
        return p.full && p.vendor != 0 ? BUILT_IN : OTHER;
    }

    /** Gets a player when it connects (else from its first input) */
    static boolean atConnect(Pad p, String chosen) { return rank(p, chosen) <= EXTERNAL; }

    /** One joystick event's values: true when one is pushed past half (a stick, trigger or hat), not a resting stick's noise */
    static boolean wakes(float[] values, int n) {
        for (int i = 0; i < n; i++) if (Math.abs(values[i]) > 0.5f) return true;
        return false;
    }

    private final Host host;
    private final Map<Integer, Pad> pads = new LinkedHashMap<>();   // gamepad-like devices connected
    private final Set<Integer> playing = new LinkedHashSet<>();   // those that have a player
    private final Map<Integer, Integer> mate = new LinkedHashMap<>();   // a paired half → its other half
    private final boolean pairHalves;
    private String chosen;

    PadOrder(Host host, String chosen, boolean pairHalves) {
        this.host = host; this.chosen = chosen == null ? "" : chosen; this.pairHalves = pairHalves;
    }

    /** The devices connected at the start: best rank first, then by id */
    void start(List<Pad> list) {
        List<Pad> l = new ArrayList<>(list);
        l.sort(Comparator.<Pad>comparingInt(p -> rank(p, chosen)).thenComparingInt(p -> p.id));
        for (Pad p : l) connected(p);
    }

    /** A gamepad-like device connected; true when it has a player now */
    boolean connected(Pad p) {
        if (pads.containsKey(p.id) && playing.contains(p.id)) return true;
        pads.put(p.id, p);
        pairUp(p);
        return atConnect(p, chosen) && join(p, false);
    }

    void removed(int id) {
        pads.remove(id); playing.remove(id);
        Integer m = mate.remove(id); if (m != null) mate.remove(m);   // the other half is a controller of its own now (the host lets it keep the player)
    }

    /** Whether the device's events go to the host: it has a player */
    boolean plays(int id) { return playing.contains(id); }

    /** Input from a gamepad-like device (a pad button, or wakes()): it gets a player if it had none. True when it has one */
    boolean active(Pad p) {
        if (playing.contains(p.id)) return true;
        pads.put(p.id, p);
        pairUp(p);
        if (playing.contains(p.id)) return true;   // its other half has a player: the pair has one
        return join(p, true);
    }

    private boolean join(Pad p, boolean byInput) {
        if (host.add(p) < 0) return false;
        playing.add(p.id);
        Integer m = mate.get(p.id); if (m != null) playing.add(m);
        takeFirst(p, byInput);
        return true;
    }

    /**
     * The other, connected half of p's controller when both halves are there and the setting is on (a left and a right Joy-Con: never the same vendor's two lefts):
     * they are paired with the host, and both count as playing when either has a player. False when there was nothing to pair
     */
    private boolean pairUp(Pad p) {
        if (!pairHalves || p.half == 0 || mate.containsKey(p.id)) return false;
        for (Pad q : pads.values()) {
            if (q.id == p.id || q.half != -p.half || mate.containsKey(q.id)) continue;
            boolean joycons = joycon(p) && joycon(q);
            // Another maker's halves only when both are external (never a phone's own device); a Joy-Con is one by its name whatever Android calls it external
            if (!joycons && !(p.external && q.external)) continue;
            if (!joycons && p.vendor != 0 && q.vendor != 0 && q.vendor != p.vendor) continue;   // another maker's half, unless Android reports no vendor for one
            Pad l = p.half < 0 ? p : q, r = p.half > 0 ? p : q;
            if (host.pair(l, r) < -1) return false;   // the host could not pair them
            mate.put(l.id, r.id); mate.put(r.id, l.id);
            if (playing.contains(l.id) || playing.contains(r.id)) { playing.add(l.id); playing.add(r.id); }
            return true;
        }
        return false;
    }

    /** Android's name for a Joy-Con, in any generation ("Joy-Con (L)", "JOYCON(R)", "Joy Con L") */
    private static boolean joycon(Pad p) {
        String n = p.name.toLowerCase(java.util.Locale.ROOT);
        return n.contains("joy-con") || n.contains("joy con") || n.contains("joycon");
    }

    /** Player 1 for p when it is free or its device ranks worse; joining by its input, also when player 1's device is no controller (BUILT_IN or OTHER) */
    private void takeFirst(Pad p, boolean byInput) {
        if (host.player(p.id) == 0) return;
        Pad one = holderOfFirst();
        int r1 = one == null ? Integer.MAX_VALUE : groupRank(one), r = groupRank(p);
        if (r1 > r || byInput && r1 >= BUILT_IN && r1 == r) host.first(p);
    }

    /** A paired device's rank is its pair's: the better of the two halves, so either half pressed counts for the whole controller */
    private int groupRank(Pad p) {
        int r = rank(p, chosen);
        Integer m = mate.get(p.id); Pad q = m == null ? null : pads.get(m);
        return q == null ? r : Math.min(r, rank(q, chosen));
    }

    private Pad holderOfFirst() {
        for (int id : playing) if (host.player(id) == 0) return pads.get(id);
        return null;
    }

    /** The gamepad-like devices connected, for the Player 1 choice: a paired pair once, as its left half */
    List<Pad> known() {
        List<Pad> out = new ArrayList<>();
        for (Pad p : pads.values()) {
            if (mate.containsKey(p.id) && p.half > 0) continue;   // the pair's left half lists it
            out.add(pairedView(p));
        }
        return out;
    }

    /** The device on player 1 as its controller (a pair's left half), null for none */
    Pad first() {
        Pad p = holderOfFirst();
        return p == null ? null : pairedView(p);
    }

    /** The pair p is part of, as one controller (the left half's descriptor and both names joined); p itself when it is not paired */
    private Pad pairedView(Pad p) {
        Integer m = mate.get(p.id);
        if (m == null) return p;
        Pad q = pads.get(m);
        return q == null ? p : pairPad(p.half < 0 ? p : q);
    }

    /** Every half of a pair a controller of its own now (the host split them, GameActivity.splitJoycons): both play, each on its player */
    void split() {
        mate.clear();
        for (Pad p : pads.values()) if (p.half != 0 && host.player(p.id) >= 0) playing.add(p.id);
    }

    /** The pair a and b, made with the host (GameActivity.mergeJoycons) */
    void link(Pad a, Pad b) {
        mate.put(a.id, b.id); mate.put(b.id, a.id);
        if (playing.contains(a.id) || playing.contains(b.id)) { playing.add(a.id); playing.add(b.id); }
    }

    /** A device's player let go (the in-game panel's Disconnect): it has none until its next press connects it again (GameActivity.disconnectPlayer) */
    void forgot(int id) {
        playing.remove(id);
        Integer m = mate.get(id); if (m != null) playing.remove(m);
    }

    /** The players with a controller (a pair as one), for the in-game panel's Controllers (GameActivity.players) */
    Map<Integer, Pad> players() {
        Map<Integer, Pad> out = new LinkedHashMap<>();
        for (Pad p : pads.values()) {
            int pl = host.player(p.id);
            if (pl < 0 || out.containsKey(pl)) continue;
            out.put(pl, pairedView(p));
        }
        return out;
    }

    /** The Pad of a device (a pair as one), null when it is not connected */
    Pad pad(int id) { Pad p = pads.get(id); return p == null ? null : pairedView(p); }

    /** A left and a right half connected and not paired, to merge (GameActivity.mergeJoycons), or null */
    Pad[] halvesToMerge() {
        Pad l = null, r = null;
        for (Pad p : pads.values()) {
            if (mate.containsKey(p.id)) continue;
            if (p.half < 0 && l == null) l = p; else if (p.half > 0 && r == null) r = p;
        }
        return l != null && r != null ? new Pad[] {l, r} : null;
    }

    /** Whether two halves are paired now (the panel offers Split) */
    boolean hasPair() { return !mate.isEmpty(); }

    /** A pair as one controller for the Player 1 choice: the left half's descriptor (either half chooses the pair) and both names joined */
    private Pad pairPad(Pad p) {
        Pad q = pads.get(mate.get(p.id));
        if (q == null) return p;
        String key = chosen.equals(q.key) ? q.key : p.key;   // a choice kept as either half's descriptor checks the pair's one entry
        return new Pad(p.id, key, pairName(p.name, q.name), p.external, p.full, p.vendor, p.half);
    }

    /** The two halves' names as one controller's, at word level: the common first and last words kept, the different middle joined with a slash ("Nintendo Switch Left/Right Joy-Con", "Joy-Con (L/R)") */
    static String pairName(String a, String b) {
        if (a == null) a = "";
        if (b == null) b = "";
        if (a.isEmpty() || b.isEmpty() || a.equals(b)) return a.isEmpty() ? b : a;
        String[] x = a.split(" "), y = b.split(" ");
        int p = 0, s = 0;
        while (p < x.length && p < y.length && x[p].equals(y[p])) p++;
        while (s < x.length - p && s < y.length - p && x[x.length - 1 - s].equals(y[y.length - 1 - s])) s++;
        String ma = words(x, p, x.length - s), mb = words(y, p, y.length - s);
        if (ma.startsWith("(") && ma.endsWith(")") && mb.startsWith("(") && mb.endsWith(")")) ma = "(" + ma.substring(1, ma.length() - 1) + "/" + mb.substring(1, mb.length() - 1) + ")";
        else ma = ma + "/" + mb;
        String pre = words(x, 0, p), suf = words(x, x.length - s, x.length);
        return (pre.isEmpty() ? "" : pre + " ") + ma + (suf.isEmpty() ? "" : " " + suf);
    }
    private static String words(String[] w, int from, int to) {
        StringBuilder s = new StringBuilder();
        for (int i = from; i < to; i++) s.append(i > from ? " " : "").append(w[i]);
        return s.toString();
    }

    /** The Player 1 choice: a descriptor ("" = automatic). The chosen device, if it is connected, becomes player 1; automatic gives it to the best-ranked one */
    void choose(String key) {
        chosen = key == null ? "" : key;
        Pad best = null;
        for (Pad p : pads.values()) {
            if (!playing.contains(p.id) && !atConnect(p, chosen)) continue;
            if (best == null || groupRank(p) < groupRank(best)) best = p;
        }
        if (best == null) return;
        if (!playing.contains(best.id)) join(best, false); else takeFirst(best, false);
    }
}
