package android.os;

/** Host-side stub: never actually sleeps, keeps the suite fast. */
public final class SystemClock {
    public static void sleep(long ms) { /* no-op */ }
}
