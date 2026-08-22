package com.miui.home.launcher;

import com.miui.home.model.api.ItemInfo;

/** Fake of the CN launcher's ShortcutInfo (test shape only). */
public class ShortcutInfo extends ItemInfo {
    private final String packageName;
    private final String title;

    public ShortcutInfo(String packageName, String title) {
        this.packageName = packageName;
        this.title = title;
    }

    public String getPackageName() { return packageName; }

    @Override
    public CharSequence getTitle() { return title; }
}
