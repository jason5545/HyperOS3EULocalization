package com.google.android.libraries.gsa.launcherclient;

/** Fake of the launcherclient bundled in CN MiuiHome: the write-once static
 *  serviceVersion cache the hooker rewrites. */
public class LauncherClient {
    private static int b = -1;

    /** Test helper. */
    public static void reset(int value) { b = value; }

    /** Test helper. */
    public static int current() { return b; }
}
