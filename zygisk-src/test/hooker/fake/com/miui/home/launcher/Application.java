package com.miui.home.launcher;

import android.content.Context;

/** Fake of the CN launcher's Application (test shape only). */
public class Application {
    private static Application sInstance;

    private final Context appContext;

    public Application(Context appContext) {
        this.appContext = appContext;
    }

    public static Application getInstance() { return sInstance; }

    public static void setInstance(Application instance) { sInstance = instance; }

    public Context getApplicationContext() { return appContext; }
}
