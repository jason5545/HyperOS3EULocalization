#!/system/bin/sh

# 移除由模組安裝到 /data/app 的更新，保留 user data，讓 ROM 內建版接回。
pm uninstall -k --user 0 com.miui.gallery >/dev/null 2>&1
pm uninstall -k --user 0 com.miui.mediaeditor >/dev/null 2>&1
pm uninstall -k --user 0 com.android.soundrecorder >/dev/null 2>&1
pm uninstall --user 0 com.android.thememanager.customthemeconfig.config.overlay >/dev/null 2>&1

# 讓 Package Manager 在移除 systemless ThemeManager 後重新掃描。
rm -rf /data/system/package_cache/*
