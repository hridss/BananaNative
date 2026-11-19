package dev.nxnative.android;

/**
 * PrepareScreen on the JVM, driven by dm/tools/tests/test_android_preparescreen.py. Commands, one per run:
 * <pre>
 * parse &lt;status or -&gt;     → "parse &lt;taken&gt; &lt;total&gt; &lt;left&gt;" or "parse null"
 * show &lt;status&gt;           → "show &lt;count&gt;|&lt;left&gt;|&lt;fraction to 3 places&gt;"
 * </pre>
 */
public final class PrepareScreenJvmTest {
    public static void main(String[] a) {
        String s = a.length > 1 && !a[1].equals("-") ? a[1] : null;
        switch (a[0]) {
            case "parse": { int[] st = PrepareScreen.parse(s); System.out.println("parse " + (st == null ? "null" : st[0] + " " + st[1] + " " + st[2])); break; }
            case "show": { int[] st = PrepareScreen.parse(s);
                System.out.println("show " + PrepareScreen.count(st) + "|" + PrepareScreen.left(st) + "|" + String.format(java.util.Locale.US, "%.3f", PrepareScreen.fraction(st))); break; }
            default: throw new IllegalArgumentException(a[0]);
        }
    }
}
