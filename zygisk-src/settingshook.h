#pragma once

#include <jni.h>

// preAppSpecialize（仍是 zygote 權限）先把 liblsplant.so 讀進記憶體；
// post 階段只需 memfd + dlopen，不在 app 權限下碰 /data/adb。
void settingshookPreloadLsplant();

// postAppSpecialize：派生 worker 線程，等 Application 出現後對
// DefaultCombinedPreferenceController.getCombinedProviderInfos 裝 lsplant
// hook（jrc.settings.CredListHooker）——只解除 Settings 憑證頁的 CN 過濾，
// 不翻轉整個 Settings。
void settingshookStartSettings(JavaVM *vm);
