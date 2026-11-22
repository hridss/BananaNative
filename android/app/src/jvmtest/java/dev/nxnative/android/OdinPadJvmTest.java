package dev.nxnative.android;

/** OdinPad on the JVM (dm/tools/tests/test_android_odinpad.py): which device is AYN's built-in controller, and when its face buttons go by label. Prints "ok" or throws */
public final class OdinPadJvmTest {
    private static void check(boolean c, String what) { if (!c) throw new AssertionError(what); }

    public static void main(String[] args) {
        final int V = OdinPad.VENDOR, P = OdinPad.PRODUCT_THOR;
        check(V == 0x2020 && P == 0x0111 && OdinPad.NAME.equals("Odin Controller"), "the identity seen on the Thor");
        check(OdinPad.STYLE_PROPERTY.equals("persist.sys.gamepad.type") && OdinPad.STYLE_XBOX.equals("1"), "the style property and its Xbox value");

        // which device
        check(OdinPad.is(V, P, "Odin Controller"), "the Thor's pad");
        check(OdinPad.is(V, P, null) && OdinPad.is(V, P, "renamed"), "the product alone is enough");
        check(OdinPad.is(V, 0x0222, "Odin Controller"), "AYN's vendor with AYN's name: another AYN handheld");
        check(!OdinPad.is(V, 0x0222, "Some Pad") && !OdinPad.is(V, 0x0222, null), "AYN's vendor alone is not enough");
        check(!OdinPad.is(0x045e, P, "Odin Controller"), "another vendor is not it, whatever it calls itself");
        check(!OdinPad.is(0x057e, 0x2009, "Pro Controller") && !OdinPad.is(0, 0, null), "other pads");
        check(!OdinPad.is(V, 0x0111 + 1, "odin controller"), "the name is matched exactly");

        // by label in the default style and whenever the style is not the Xbox value
        for (String style : new String[] {"0", "", null, " 0 ", "2", "xbox", "01", "1x"})
            check(OdinPad.labelled(V, P, "Odin Controller", style), "by label in style [" + style + "]");
        // by position (the host's default) in the Xbox style
        check(!OdinPad.labelled(V, P, "Odin Controller", "1"), "Xbox style: the default mapping");
        check(!OdinPad.labelled(V, P, "Odin Controller", " 1\n"), "the value is trimmed");
        // never for another pad
        for (String style : new String[] {"0", "1", "", null})
            check(!OdinPad.labelled(0x045e, 0x028e, "Xbox 360 Controller", style) && !OdinPad.labelled(V, 0x0222, "Some Pad", style),
                  "another pad keeps the default in style [" + style + "]");

        // off Android the platform class is absent: the style reads as "", never throws
        check(OdinPad.style().isEmpty(), "no platform class: an empty style");
        check(OdinPad.labelled(V, P, "Odin Controller", OdinPad.style()), "an unreadable style counts as the default one");
        System.out.println("ok");
    }
}
