package dev.nxnative.android;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

/**
 * PadOrder on the JVM (dm/tools/tests/test_android_padorder.py) against a stand-in for the host's players (pad_slots.h and pad_pair.h: the lowest free player on
 * connecting, pad_slot_first trading player 1, a left and a right half of one controller on one player). Devices as a POCO F7 Ultra with a GameSir X3 Pro (issues
 * #42, #62; the ids from its log, uinput-xiaomi's other facts assumed both ways), a handheld's built-in pad, Joy-Cons and others. Prints "ok" or throws
 */
public final class PadOrderJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }

    /** The host's players: owner[p] is the device id on player p + 1, -1 for none; mate: a paired half's other half (pad_pair.h) */
    static final class Slots implements PadOrder.Host {
        final int[] owner = new int[8];
        final Map<Integer, Integer> mate = new HashMap<>();
        int firsts;
        Slots() { Arrays.fill(owner, -1); }
        private int slot(int id) { for (int i = 0; i < owner.length; i++) if (owner[i] == id) return i; return -1; }
        @Override public int player(int id) {
            int p = slot(id);
            Integer m = mate.get(id);
            if (p < 0 && m != null) p = slot(m);
            return p;
        }
        @Override public int add(PadOrder.Pad p) {
            int had = player(p.id); if (had >= 0) return had;
            for (int i = 0; i < owner.length; i++) if (owner[i] < 0) { owner[i] = p.id; return i; }
            return -1;
        }
        @Override public int first(PadOrder.Pad p) {
            int at = add(p); if (at < 0) return -1;
            firsts++;
            int was = owner[0]; owner[0] = owner[at]; owner[at] = was;
            return 0;
        }
        @Override public int pair(PadOrder.Pad l, PadOrder.Pad r) {
            if (l.id < 0 || r.id < 0 || l.id == r.id) return -2;
            mate.put(l.id, r.id); mate.put(r.id, l.id);
            int pl = player(l.id), pr = player(r.id);
            if (pl >= 0 && pr >= 0 && pl != pr) owner[pr] = -1;   // both had one: the left keeps its own
            return pl >= 0 ? pl : pr;
        }
    }

    static final PadOrder.Pad XIAOMI = new PadOrder.Pad(3, "d-uinput", "uinput-xiaomi", false, false, 0, 0),        // the phone's own: built in, no vendor
            XIAOMI_FULL = new PadOrder.Pad(3, "d-uinput", "uinput-xiaomi", false, true, 0, 0),                        // the same, should it report buttons and a stick
            GAMESIR = new PadOrder.Pad(22, "d-gamesir", "Sony Computer Entertainment GameSir-X3 Pro", true, true, 0x054c, 0),
            XBOX = new PadOrder.Pad(30, "d-xbox", "Xbox Wireless Controller", true, true, 0x045e, 0),
            BUILT_IN = new PadOrder.Pad(5, "d-builtin", "Handheld Controller", false, true, 0x2020, 0),
            KEYBOARD = new PadOrder.Pad(40, "d-kbd", "Bluetooth keyboard", true, false, 0x05ac, 0),
            JOYCON_L = new PadOrder.Pad(50, "d-jc-l", "Joy-Con (L)", true, false, 0x057e, PadOrder.HALF_LEFT),
            JOYCON_R = new PadOrder.Pad(51, "d-jc-r", "Joy-Con (R)", true, true, 0x057e, PadOrder.HALF_RIGHT),
            JOYCON_L2 = new PadOrder.Pad(52, "d-jc-l2", "Joy-Con (L)", true, false, 0x057e, PadOrder.HALF_LEFT),
            JOYCON_R2 = new PadOrder.Pad(53, "d-jc-r2", "Joy-Con (R)", true, true, 0x057e, PadOrder.HALF_RIGHT),
            PRO = new PadOrder.Pad(54, "d-pro", "Pro Controller", true, true, 0x057e, 0);

    public static void main(String[] args) {
        check(PadOrder.rank(GAMESIR, "") == PadOrder.CONTROLLER && PadOrder.rank(XIAOMI, "") == PadOrder.OTHER && PadOrder.rank(XIAOMI_FULL, "") == PadOrder.OTHER, "ranks");
        check(PadOrder.rank(BUILT_IN, "") == PadOrder.BUILT_IN && PadOrder.rank(KEYBOARD, "") == PadOrder.EXTERNAL && PadOrder.rank(XIAOMI, "d-uinput") == PadOrder.CHOSEN, "ranks 2");
        check(!PadOrder.wakes(new float[]{0.1f, -0.2f, 0.5f}, 3) && PadOrder.wakes(new float[]{0f, -0.8f}, 2) && !PadOrder.wakes(new float[]{0f, 0.9f}, 1), "wakes");

        // #42 / #62: the phone's device has the lower id and was player 1; now the controller is, and the phone's device has no player until it sends input
        Slots h = new Slots(); PadOrder o = new PadOrder(h, "", true);
        o.start(Arrays.asList(XIAOMI, GAMESIR));
        check(h.player(22) == 0 && h.player(3) == -1 && !o.plays(3) && o.plays(22), "the controller is player 1, the phone's device none");
        check(o.first() == GAMESIR && o.known().size() == 2, "first, known");
        check(o.active(XIAOMI) && h.player(3) == 1 && h.player(22) == 0, "the phone's device sending input: player 2, the controller stays player 1");

        h = new Slots(); o = new PadOrder(h, "", true);   // the same if it reports buttons and a stick
        o.start(Arrays.asList(XIAOMI_FULL, GAMESIR));
        check(h.player(22) == 0 && h.player(3) == -1, "full uinput: the controller is player 1");

        // No controller: the phone's device woke, then a controller connects: it takes player 1
        h = new Slots(); o = new PadOrder(h, "", true);
        o.start(Arrays.asList(XIAOMI));
        check(h.player(3) == -1, "alone, silent: no player");
        o.active(XIAOMI);
        check(h.player(3) == 0, "alone, input: player 1");
        o.connected(GAMESIR);
        check(h.player(22) == 0 && h.player(3) == 1, "a controller connecting takes player 1 from it");
        o.connected(XBOX);
        check(h.player(30) == 2 && h.player(22) == 0, "a second controller is player 3, not player 1");
        o.removed(22); h.owner[0] = -1;   // the host frees its player (nxhost_input_device_removed)
        o.connected(GAMESIR);
        check(h.player(22) == 0, "back: player 1 free again");

        // Two external controllers at the start: the order of their ids
        h = new Slots(); o = new PadOrder(h, "", true);
        o.start(Arrays.asList(XBOX, KEYBOARD, GAMESIR, XIAOMI));
        check(h.player(22) == 0 && h.player(30) == 1 && h.player(40) == 2 && h.player(3) == -1, "controllers by id, then other external devices");
        check(h.firsts == 0, "nothing traded at the start in rank order");

        // A handheld: its built-in pad gets player 1 from its first press, also over the phone's device that woke first
        h = new Slots(); o = new PadOrder(h, "", true);
        o.start(Arrays.asList(BUILT_IN, XIAOMI));
        check(h.player(5) == -1 && h.player(3) == -1, "built in: no player before input");
        o.active(XIAOMI); o.active(BUILT_IN);
        check(h.player(5) == 0 && h.player(3) == 1, "the built-in pad takes player 1 from the phone's device");
        o.connected(GAMESIR);
        check(h.player(22) == 0, "an external controller still comes first");

        // Two devices that are no controller: the one used second takes player 1 (no controller on player 1)
        PadOrder.Pad other = new PadOrder.Pad(9, "d-other", "Other", false, false, 0, 0);
        h = new Slots(); o = new PadOrder(h, "", true);
        o.start(Arrays.asList(XIAOMI, other));
        o.active(XIAOMI); o.active(other);
        check(h.player(9) == 0 && h.player(3) == 1, "used by the player: player 1");

        // The Player 1 choice: by descriptor, at the start and at once; automatic again leaves a controller where it is
        h = new Slots(); o = new PadOrder(h, "d-xbox", true);
        o.start(Arrays.asList(GAMESIR, XBOX));
        check(h.player(30) == 0 && h.player(22) == 1, "chosen: player 1 at the start");
        o.choose("d-gamesir");
        check(h.player(22) == 0 && h.player(30) == 1, "chosen now: they trade");
        o.choose("");
        check(h.player(22) == 0, "automatic: two controllers, nothing moves");
        o.choose("d-uinput");   // not connected: nothing changes
        check(h.player(22) == 0, "a chosen device not connected");
        o.connected(XIAOMI);
        check(h.player(3) == 0 && h.player(22) == 2, "the chosen device connects: player 1, even built in");
        o.choose("");
        check(h.player(22) == 0 || h.player(30) == 0, "automatic again: a controller on player 1");

        h = new Slots(); o = new PadOrder(h, "", true);   // choosing a silent device gives it a player
        o.start(Arrays.asList(XIAOMI, GAMESIR));
        o.choose("d-uinput");
        check(h.player(3) == 0 && h.player(22) == 1 && o.plays(3), "a silent device chosen: player 1");

        // Two Joy-Cons: the halves of one controller, one player, as in a grip (PadOrder.pairUp; the host's side is pad_pair.h in t_hid.c)
        h = new Slots(); o = new PadOrder(h, "", true);
        o.start(Arrays.asList(JOYCON_L, JOYCON_R));
        check(h.player(50) == 0 && h.player(51) == 0 && o.plays(50) && o.plays(51), "a left and a right Joy-Con: one player for both");
        check(o.first() == JOYCON_R || o.first() == JOYCON_L, "the pair is player 1");
        o.active(JOYCON_L);
        check(h.owner[1] == -1, "either half's input: no second player");

        h = new Slots(); o = new PadOrder(h, "", true);   // a pair takes player 1 from the phone's device, as a controller does
        o.start(Arrays.asList(XIAOMI, JOYCON_L, JOYCON_R));
        check(h.player(50) == 0 && h.player(51) == 0 && h.player(3) == -1, "the pair is player 1, the phone's device none");

        h = new Slots(); o = new PadOrder(h, "", false);   // the setting off: one controller each, for two players with one Joy-Con each
        o.start(Arrays.asList(JOYCON_L, JOYCON_R));
        check(h.player(51) == 0 && h.player(50) == 1 && !h.mate.containsKey(50), "off: the right Joy-Con player 1, the left player 2");

        h = new Slots(); o = new PadOrder(h, "", true);   // two pairs: four controllers, two players
        o.start(Arrays.asList(JOYCON_L, JOYCON_R, JOYCON_L2, JOYCON_R2));
        check(h.player(50) == h.player(51) && h.player(52) == h.player(53) && h.player(50) >= 0 && h.player(52) >= 0 && h.player(50) != h.player(52),
                "two pairs, two players");

        h = new Slots(); o = new PadOrder(h, "", true);   // the same vendor's other halves never pair with each other
        o.start(Arrays.asList(JOYCON_L, JOYCON_L2));
        check(h.player(50) >= 0 && h.player(52) >= 0 && h.player(50) != h.player(52) && h.mate.isEmpty(), "two left halves: two players");
        o.connected(PRO);
        check(h.mate.isEmpty() && h.player(54) >= 0, "a whole controller is never half of a pair");
        System.out.println("ok");
    }
}