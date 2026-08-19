// Dual wake（小愛 + Hey Google 並存）Zygisk 支援。
//
// 兩個獨立功能，都只在自己的目標進程啟動：
//
// 1. CoreAlive（com.miui.voiceassist:voice_trigger）
//    Google 是預設助理時，Android 不會啟動小愛的 VoiceInteractionService，
//    308 的 CoreAliveManager.registerAlive() 因此沒人呼叫。這裡在
//    postAppSpecialize 後等 Application 可用，注入內嵌 dex 的
//    jrc.dualwake.CoreAliveBridge，在在主執行緒呼叫官方
//    CoreAliveManager.a.registerAlive(context, null)，讓系統自己綁定
//    VoiceTriggerService。不用 shell、不改 role、不清資料。
//
// 2. VoiceTrigger restart 穩定化（com.miui.voicetrigger）
//    以 LSPlant 對 com.miui.voicetrigger.wakeup.q.k(Ljava/lang/String;)I
//    （restartRecognition）做真正的 ART method hook，行為比照原
//    HyperCeiler StabilizeRecognitionRestart：750ms 內同 session 重複
//    restart 直接回 0；原方法回 -38 改成 0；0/-38 時記錄成功時間。
//    liblsplant.so 只在這個進程經 memfd dlopen，不進其他 app 的 maps。

#pragma once

#include <jni.h>

// preAppSpecialize（仍是 zygote 權限）時預讀 liblsplant.so 到位記憶體。
// 只有 com.miui.voicetrigger 進程需要呼叫；失敗時安全 no-op。
void dualwakePreloadLsplant();

// postAppSpecialize：啟動 CoreAlive worker（voiceassist:voice_trigger）。
void dualwakeStartCoreAlive(JavaVM *vm);

// postAppSpecialize：啟動 VoiceTrigger restart hook worker（voicetrigger）。
void dualwakeStartVoiceTrigger(JavaVM *vm);
