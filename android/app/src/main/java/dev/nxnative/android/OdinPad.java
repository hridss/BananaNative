package dev.nxnative.android;

/**
 * AYN's built-in controller, which calls itself "Odin Controller" on the Thor and the Odin handhelds.
 *
 * Seen on an AYN Thor (2026-11-20, dumpsys input): bus 0x0003, vendor 0x2020, product 0x0111, name "Odin Controller",
 * key layout Vendor_2020_Product_0111.kl. In the system's default Controller style it reports its face buttons by the
 * letters printed on them, which are Nintendo's (A on the right): Android's BUTTON_A is the right button. The host's
 * default maps a pad by position, as an Xbox pad needs (input_map.h), which trades A with B and X with Y on this pad.
 * So this pad is added as "labelled" (Native.deviceAdded), unless the system's Controller style is Xbox.
 *
 * The Controller style lives in the system property persist.sys.gamepad.type (AYN's settings, game assistant, touch
 * mapping and system UI all name it; the Thor read 0 in its default style). That Xbox style is the value 1, and that
 * the pad then reports by position, is inferred from AYN's own strings ("Flipped Gamepad", "XBox Style") and has NOT
 * been seen on a device: in that style this class leaves the host's default alone, which is what happened before.
 * Only the Thor was looked at; an Odin 2 or 3 is matched by the vendor with the same name, not by a product seen.
 * DM_PAD_LABELLED (Game settings, Xbox button labels) and Button mapping still win over this (input_android.c take).
 */
final class OdinPad {
    static final int VENDOR = 0x2020, PRODUCT_THOR = 0x0111;
    static final String NAME = "Odin Controller";
    static final String STYLE_PROPERTY = "persist.sys.gamepad.type";
    static final String STYLE_XBOX = "1";

    private OdinPad() {}

    /** AYN's built-in controller: its vendor, with the product seen on the Thor or the name AYN gives it */
    static boolean is(int vendor, int product, String name) {
        return vendor == VENDOR && (product == PRODUCT_THOR || NAME.equals(name));
    }

    /** The face buttons of this device go by label (Native.deviceAdded's labelled): AYN's pad, outside the Xbox style */
    static boolean labelled(int vendor, int product, String name, String style) {
        return is(vendor, product, name) && !STYLE_XBOX.equals(style == null ? "" : style.trim());
    }

    /** The system's Controller style ("" when it cannot be read: off an AYN device, or where the platform class is absent) */
    static String style() {
        try {
            Object v = Class.forName("android.os.SystemProperties").getMethod("get", String.class, String.class).invoke(null, STYLE_PROPERTY, "");
            return v instanceof String ? (String) v : "";
        } catch (ReflectiveOperationException | RuntimeException e) {
            return "";
        }
    }
}
