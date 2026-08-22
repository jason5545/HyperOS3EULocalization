package com.miui.home.launcher;

/** Fake of the CN launcher's LauncherAssistantCompat (test shape only). */
public abstract class LauncherAssistantCompat {
    protected final BaseLauncher mLauncher;
    protected final String mPackageName;

    LauncherAssistantCompat(BaseLauncher launcher, String packageName) {
        this.mLauncher = launcher;
        this.mPackageName = packageName;
    }
}
