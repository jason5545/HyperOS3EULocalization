package android.util;

import java.util.ArrayList;
import java.util.List;

/** Host-side stub: records log lines instead of writing logcat. */
public final class Log {
    public static final List<String> lines = new ArrayList<>();

    public static int i(String tag, String msg) { lines.add("I/" + tag + ": " + msg); return 0; }
    public static int w(String tag, String msg) { lines.add("W/" + tag + ": " + msg); return 0; }
    public static int w(String tag, String msg, Throwable t) { lines.add("W/" + tag + ": " + msg); return 0; }
    public static int e(String tag, String msg) { lines.add("E/" + tag + ": " + msg); return 0; }
    public static int e(String tag, String msg, Throwable t) { lines.add("E/" + tag + ": " + msg); return 0; }
}
