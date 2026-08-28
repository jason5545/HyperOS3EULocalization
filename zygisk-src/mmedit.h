#pragma once

#include <jni.h>

// preAppSpecialize（仍是 zygote 權限）先把 liblsplant.so 讀進記憶體；
// post 階段只需 memfd + dlopen，不在 app 權限下碰 /data/adb。
void mmeditPreloadLsplant();

// postAppSpecialize：派生 worker 線程，等 Application 出現後對
// SystemProperties.get(String) 裝 lsplant hook（jrc.mmedit.RegionHooker）。
void mmeditStartEditor(JavaVM *vm);
