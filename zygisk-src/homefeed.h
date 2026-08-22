// MiuiHome CN Google 負一屏 Zygisk 支援。
//
// CN 版系統桌面（7.50.06.2529）的 Google minus screen 由
// DeviceConfig.isUseGoogleMinusScreen() 把關：要求 ro.com.miui.rsa /
// ro.com.miui.rsa.search 命中運營商清單，或 ro.miui.customized.region 在
// {mx_telcel, lm_cr} 內。EU 裝置這些 prop 全是空值，因此負一屏資料來源
// 永遠沒有 Google Feed。
//
// 這裡在 com.miui.home 進程內用 LSPlant hook framework 的
// android.os.SystemProperties.get(String)（CN 桌面經
// com.miui.launcher.utils.SystemProperties 反射呼叫此唯一入口），
// 只把 ro.com.miui.rsa 謊報為 tier1_5，其餘 key 原樣轉發。
// 不動全域 prop，不影響任何其他 app；liblsplant.so 只在這個進程經
// memfd dlopen，不進其他 app 的 maps。

#pragma once

#include <jni.h>

// preAppSpecialize（仍是 zygote 權限）時預讀 liblsplant.so 到位記憶體。
// 只有 com.miui.home 進程需要呼叫；失敗時安全 no-op。
void homefeedPreloadLsplant();

// postAppSpecialize：啟動 prop hook worker（com.miui.home）。
void homefeedStartMiuiHome(JavaVM *vm);
