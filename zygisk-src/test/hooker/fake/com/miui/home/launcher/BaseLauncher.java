package com.miui.home.launcher;

/** Fake of the CN launcher's BaseLauncher (test shape only). */
public class BaseLauncher {
    public int closeFolderCalls;
    public boolean lastCloseFolderArg;

    public void closeFolder(boolean animate) {
        closeFolderCalls++;
        lastCloseFolderArg = animate;
    }
}
