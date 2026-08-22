package android.os;

/** Host-side stub: exists so shouldInstall's Class.forName probe succeeds. */
public final class SystemProperties {
    public static String get(String key) { return ""; }
}
