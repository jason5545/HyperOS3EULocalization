# HyperOS 3 EU 小愛・傳送門・Mi Pay

這是從 [LSHFGJ/HyperOS3EULocalization](https://github.com/LSHFGJ/HyperOS3EULocalization) 精簡出的固定功能版本，目標是 xiaomi.eu HyperOS 3。

安裝時不需要用音量鍵選功能。模組只會掛載下面九個 App payload：

| 功能 | Package | 路徑 |
| --- | --- | --- |
| 小愛語音 | `com.miui.voiceassist` | `system/product/app/VoiceAssistAndroidT` |
| 小愛視覺 | `com.xiaomi.aiasst.vision` | `system/product/app/AiAsstVision` |
| 小愛服務 | `com.xiaomi.aiasst.service` | `system/product/app/MIUIAiasstService` |
| 傳送門 | `com.miui.contentextension` | `system/product/priv-app/MIUIContentExtension` |
| Mi Pay / NextPay | `com.miui.nextpay` | `system/product/app/MINextpay` |
| 小米智慧卡 | `com.miui.tsmclient` | `system/product/app/MITSMClient` |
| 小米錢包 | `com.mipay.wallet` | `system/product/app/MipayWallet` |
| 銀聯 TSM | `com.unionpay.tsmservice.mi` | `system/product/app/UPTsmService` |
| 小米支付服務 | `com.xiaomi.payment` | `system/product/app/PaymentService` |

不會額外安裝負一屏、簡訊、黃頁、GetApps 或快應用，也不會建立原版的 CleanMaster 空白覆蓋檔。

## Taplus 長按修復

這台 xiaomi.eu ROM 的 `miui.contentcatcher.InterceptorProxy.create(Activity)` 會在國際版判斷成立時直接回傳 `null`，所以傳送門雖然已開啟，App 內長按仍不會建立攔截器。

v1.0.2 內建已在這台裝置驗證過的 Zygisk hook：只在非排除 App 的進程內把 `miui.os.Build.IS_INTERNATIONAL_BUILD` 改為 `false`。它不改全域 prop、不碰 `system_server`，也不會把整台 ROM 切成中國版。預設排除 Launcher 與負一屏，清單在模組根目錄的 `excluded_packages.txt`。

這個修復需要 Zygisk Next 正常啟用。標準 Android `TextView` 已驗證可觸發；YouTube Litho 這類虛擬文字節點仍超出 MIUI 原生擷取器能力，這不是手勢設定造成的。

## App 語系

開機後會把小愛語音、視覺與服務指定為 `zh-CN`；傳送門、NextPay、小米智慧卡、小米錢包、銀聯 TSM 與支付服務指定為 `zh-TW`。這是每個 App 的語系設定，不會改系統語言或 ROM 地區。

## 會寫入的系統屬性

模組只會透過 `system.prop` 補上 Mi Pay 與小愛需要、但這包 ROM 目前沒有提供的兩個功能開關：

```properties
ro.se.type=eSE,HCE,UICC
ro.vendor.audio.aiasst.support=true
```

`ro.product.mod_device`、`ro.miui.region` 與 Mi Push cache 都不修改，完整沿用 ROM 與目前使用者地區。模組也不包含先前測試過、後來確認這台 ROM 不需要的 `FocusXmsOverlay`。

## 安裝

1. 確認裝置是 xiaomi.eu HyperOS 3，並保留可進入 Recovery / Fastboot 的回復方式。
2. KernelSU 使用者先啟用可用的 systemless 掛載元模組，例如 `magic_mount_rs`。
3. 從 Magisk、KernelSU、SukiSU 或 APatch 的模組管理器安裝 ZIP。
4. 重新開機。

KernelSU / SukiSU 如果有針對個別 App 啟用 `Umount modules`，至少要讓下面這些 UID / package 看得到模組掛載：

```text
android.uid.system
android.uid.nfc
android.uid.phone
com.miui.nextpay
com.miui.contentextension
com.xiaomi.payment
com.miui.voiceassist
com.mipay.wallet
com.unionpay.tsmservice.mi
```

不要和原版 `HyperOS3EULocalization` 同時啟用；兩者會掛載相同路徑，安裝器偵測到原版仍啟用時會中止。

若先前已安裝獨立的 `taplus_intl_fix`，裝好 v1.0.2 後應移除舊模組。兩支 hook 同時存在不會改全域屬性，但沒有必要讓每個 App process 重複載入。

## 建置

```sh
./build.sh
```

ZIP 會產生在 `dist/`。建置腳本會驗證固定的九個 App payload、Taplus arm64 Zygisk binary 與排除清單，並拒絕把多餘 App 包進去。

## 授權與來源

沿用上游的 GPL-3.0 授權。原始專案也承襲自 MinaMichita/MiuiEULocalizationToolsBox。
