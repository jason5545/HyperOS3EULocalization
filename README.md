# HyperOS 3 EU 小愛・傳送門・智慧卡・國行媒體

這是 [LSHFGJ/HyperOS3EULocalization](https://github.com/LSHFGJ/HyperOS3EULocalization) 的量身精簡版，供 xiaomi.eu HyperOS 3 使用。

它補回小愛、傳送門、智慧卡需要的元件，加入國行相簿、相簿編輯器、錄音機與主題商店，並修正國際版 ROM 無法觸發 Taplus 長按的問題。沒有音量鍵選單，也不會順手裝回負一屏、簡訊、黃頁、GetApps、快應用或小米錢包。

目前版本是 `v1.0.2`。實機開發與驗證環境為 Xiaomi myron、xiaomi.eu `OS3.0.308.0.WPMCNXM`、Android 16、KernelSU 與 Zygisk Next。

## 模組內容

ZIP 內固定包含十二個 App payload：

| 功能 | Package | 模組路徑 |
| --- | --- | --- |
| 小愛語音 | `com.miui.voiceassist` | `system/product/app/VoiceAssistAndroidT` |
| 小愛視覺 | `com.xiaomi.aiasst.vision` | `system/product/app/AiAsstVision` |
| 小愛服務 | `com.xiaomi.aiasst.service` | `system/product/app/MIUIAiasstService` |
| 傳送門 | `com.miui.contentextension` | `system/product/priv-app/MIUIContentExtension` |
| NextPay | `com.miui.nextpay` | `system/product/app/MINextpay` |
| 小米智慧卡 | `com.miui.tsmclient` | `system/product/app/MITSMClient` |
| 銀聯 TSM | `com.unionpay.tsmservice.mi` | `system/product/app/UPTsmService` |
| 小米支付服務 | `com.xiaomi.payment` | `system/product/app/PaymentService` |
| 國行相簿 | `com.miui.gallery` | `system/product/priv-app/MiuiGallery` |
| 國行相簿編輯器 | `com.miui.mediaeditor` | `system/product/app/MiMediaEditor` |
| 國行錄音機 | `com.android.soundrecorder` | `system/product/priv-app/SoundRecorder` |
| 國行主題商店 | `com.android.thememanager` | `system/product/app/ThemeManager` |

這四個國行 App 來自 [mikal/HyperOS3EULocalization 的更新部分](https://github.com/mikal/HyperOS3EULocalization#%E6%9B%B4%E6%96%B0%E9%83%A8%E5%88%86)。ThemeManager 沒有照抄對方的 `/system/app` 路徑，而是配合 MYRON 實機原始位置放在 `/product/app`，避免同包名兩份 APK 並存。

### 國行 App 簽章與版本

MYRON xiaomi.eu 內建四個國際版 App 的簽章 SHA-256 是 `f87bd41b…`，這組國行 APK 是小米簽章 `c9009d01…`。本機需要 CorePatch 在 LSPosed 的 `system` scope 啟用下列相容功能：

- 允許不同簽章取代現有 App
- 允許 shared UID 簽章不同（ThemeManager 使用 `android.uid.theme`）
- 允許降版

開機服務會偵測 `org.lsposed.corepatch`：

- 有 CorePatch：維持本版的 systemless 覆蓋，不另外安裝 APK，也不刪 App 資料。
- 沒有 CorePatch：改走 mikal 的開機後 `pm install -r -d -g` 補裝流程，每個系統版本只執行一次，結果寫在模組目錄的 `cn_media_install.log`。這條 fallback 可處理簽章本來就相容的 ROM；若 ROM 與國行 APK 簽章不同，Android 本身還是會拒絕安裝，log 會明確記錄 `FAILED`。

MYRON `OS3.0.308.0.WPMCNXM` 實機比對：

| App | 模組國行版 | ROM 原版 | 變化 |
| --- | --- | --- | --- |
| Gallery | `5.0.3.11-0318-R` | `4.3.1.16-global` | 升級 |
| MediaEditor | `2.3.0.4.1` | `2.4.0.4.3-global` | 降版 |
| SoundRecorder | `7.8.6` | `7.8.9.9-643c0d7ef` | 降版 |
| ThemeManager | `10.6.8.0` | `10.8.7.6` | 降版 |

安裝器會清掉 Package Manager cache，但不會主動刪除這四個 App 的 user data。若降版後某個 App 因舊資料庫閃退，再只清該 App 的資料，不需要一次清四個。

mikal 版已知問題也保留：相簿編輯器的「智慧去人」使用數小時後可能閃退。

### 沒有小米錢包

`com.mipay.wallet` 已從 repo 與 ZIP 移除。智慧卡卡包、門卡、交通卡、車鑰匙與基礎 NFC 元件仍在；銀行卡、Mi Pay 錢包入口、雙擊電源開啟 Mi Pay，以及部分依賴錢包的儲值流程不可用。

這不是隱藏圖示或停用 Activity。模組裡沒有小米錢包 APK。

## Taplus 長按修復

這版 xiaomi.eu 的 `miui.contentcatcher.InterceptorProxy.create(Activity)` 會在 `miui.os.Build.IS_INTERNATIONAL_BUILD` 為 `true` 時直接回傳 `null`。傳送門設定看起來正常，App 內卻根本沒有建立長按攔截器。

模組內的 arm64 Zygisk hook 會在 App process 啟動後，把該 process 的 `IS_INTERNATIONAL_BUILD` 改為 `false`。它不修改 `ro.product.mod_device`、不改 `ro.miui.region`，也不碰 `system_server`。

這個作法會讓同一個 App 內其他國際版判斷一起改變，所以不是所有 Xiaomi App 都適合 flip。`excluded_packages.txt` 目前排除：

| Package | 保留國際版判斷的原因 |
| --- | --- |
| `com.miui.home` | 避免 Launcher 跟著切換中國版行為 |
| `com.miui.personalassistant` | 避免負一屏內容來源與入口改變 |
| `com.android.contacts` | 避免聯絡人切換中國區行為 |
| `com.miui.weather2` | 實機確認 flip 後資料源會從 AccuWeather 變成彩雲 |
| `com.android.calendar` | 日曆本身會依國際版旗標改變功能與服務判斷 |
| `com.miui.packageinstaller` | 避免啟用中國 Market、病毒掃描與安裝器雲端設定 |
| `com.miui.securitycenter` | 避免防毒雲掃描、反詐、垃圾訊息與流量服務一起切到中國版邏輯 |
| `com.android.phone` | 避免電話、SIM 與電信功能套用中國版判斷 |
| `com.android.systemui` | 避免狀態列、通知、控制中心與鎖定畫面套用中國版判斷 |

修改排除清單後不用重新刷模組，但必須重新啟動目標 App process；已經執行中的 process 不會自動還原欄位值。

標準 Android `TextView` 已驗證可以觸發 Taplus。YouTube Litho 這類虛擬文字節點不在 MIUI 原生擷取器的可見範圍內，改手勢模式或 flip 國際版旗標都無法補上。

## App 語系

開機服務會設定個別 App 語系，不會改系統語言：

- 小愛語音、小愛視覺、小愛服務：`zh-CN`
- 傳送門、NextPay、小米智慧卡、銀聯 TSM、支付服務：`zh-TW`
- 國行相簿、相簿編輯器、錄音機、主題商店：`zh-TW`

模組不修改 Launcher 顯示名稱，也不重簽 APK。若 Activity 本身沒有 `android:label`，HyperOS 仍可能顯示完整 class 名稱。

## 系統屬性

`system.prop` 只補兩個功能開關：

```properties
ro.se.type=eSE,HCE,UICC
ro.vendor.audio.aiasst.support=true
```

不修改：

- `ro.product.mod_device`
- `ro.miui.region`
- Mi Push 地區與 cache
- ROM 內原有的 Focus／XMS 白名單

模組不包含 `FocusXmsOverlay`。

## 安裝條件

- xiaomi.eu HyperOS 3
- Magisk、KernelSU、SukiSU 或 APatch
- 可用的 systemless 掛載能力；KernelSU 可使用 `magic_mount_rs` 等掛載元模組
- Zygisk Next；沒有它時 App payload 仍可掛載，但 Taplus 國際版修復不會生效
- MYRON 上需 CorePatch，並在 LSPosed 對 `system` 啟用簽章、shared UID 與降版相容開關
- arm64 裝置

不要和原版 `HyperOS3EULocalization` 同時啟用，兩者會掛載相同路徑。安裝器偵測到原版仍啟用時會中止。

若裝置上還有獨立的 `taplus_intl_fix`，請移除舊模組。v1.0.2 已經內建相同 hook，沒有必要讓每個 App process 載入兩次。

## 安裝

1. 從 root 管理器安裝模組 ZIP。
2. 重新開機。
3. 確認小愛、傳送門、智慧卡與四個國行 App 都已被系統載入。

KernelSU／SukiSU 若對個別 App 啟用了 `Umount modules`，下列 UID 或 package 至少要能看到模組掛載：

```text
android.uid.system
android.uid.nfc
android.uid.phone
com.miui.nextpay
com.miui.contentextension
com.xiaomi.payment
com.miui.voiceassist
com.unionpay.tsmservice.mi
```

## 建置

直接建立模組 ZIP：

```sh
./build.sh
```

輸出位於 `dist/`。建置腳本會檢查十二個固定 payload、arm64 Zygisk binary 與排除清單；只要出現小米錢包或其他明確排除的 App，建置就會中止。

重新編譯 Zygisk hook：

```sh
NDK_BUILD=/path/to/ndk-build ./scripts/build_zygisk.sh
```

## 上游與授權

這個 repo 保留 GitHub fork 關係，主要修改集中在固定 payload、Taplus Zygisk hook、App 語系與無互動安裝流程。四個國行媒體 App 來自 mikal 的 fork，並依 MYRON 實際系統路徑調整 ThemeManager。

專案沿用上游的 GPL-3.0 授權。原始工作可追溯至 LSHFGJ/HyperOS3EULocalization 與 MinaMichita/MiuiEULocalizationToolsBox。
