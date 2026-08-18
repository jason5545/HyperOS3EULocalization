# HyperOS 3 EU 小愛・語音喚醒・AI 通話・傳送門・智慧卡・國行媒體

這是 [LSHFGJ/HyperOS3EULocalization](https://github.com/LSHFGJ/HyperOS3EULocalization) 的量身精簡版，供 xiaomi.eu HyperOS 3 使用。

它補回小愛、語音喚醒、小米語音引擎、AI 通話、傳送門、智慧卡需要的元件，加入國行相簿、相簿編輯器、錄音機與主題商店，並修正國際版 ROM 無法觸發 Taplus 長按的問題。沒有音量鍵選單，也不會順手裝回負一屏、簡訊、黃頁、GetApps、快應用或小米錢包。

目前版本是 `v1.0.6`。實機開發與驗證環境為 POCO F8 Ultra（myron）、xiaomi.eu `OS3.0.308.0.WPMCNXM`、Android 16、KernelSU 與 Zygisk Next。

## 模組內容

ZIP 內固定包含十四個 App payload，另有小愛預設助理與 ThemeManager CN 兩個 overlay：

| 功能 | Package | 模組路徑 |
| --- | --- | --- |
| 小愛語音 | `com.miui.voiceassist` | `system/product/app/VoiceAssistAndroidT` |
| 小愛語音喚醒 | `com.miui.voicetrigger` | `system/product/app/VoiceTrigger` |
| 小米系統語音引擎 | `com.xiaomi.mibrain.speech` | `payload/xiaoai/MIUIXiaoAiSpeechEngine.apk` |
| 小愛預設助理 overlay | `com.miui.voiceassistoverlay` | `system/product/overlay/VoiceAssistAndroidOverlay` |
| 小愛視覺 | `com.xiaomi.aiasst.vision` | `system/product/app/AiAsstVision` |
| 小愛服務／AI 通話 | `com.xiaomi.aiasst.service` | `system/product/app/MIUIAiasstService` |
| 傳送門 | `com.miui.contentextension` | `system/product/priv-app/MIUIContentExtension` |
| NextPay | `com.miui.nextpay` | `system/product/app/MINextpay` |
| 小米智慧卡 | `com.miui.tsmclient` | `system/product/app/MITSMClient` |
| 銀聯 TSM | `com.unionpay.tsmservice.mi` | `system/product/app/UPTsmService` |
| 小米支付服務 | `com.xiaomi.payment` | `system/product/app/PaymentService` |
| 國行相簿 | `com.miui.gallery` | `payload/cn-media/MiuiGallery.apk` |
| 國行相簿編輯器 | `com.miui.mediaeditor` | `payload/cn-media/MiMediaEditor.apk` |
| 國行錄音機 | `com.android.soundrecorder` | `payload/cn-media/SoundRecorder.apk` |
| 國行主題商店 | `com.android.thememanager` | `system/product/app/ThemeManager` |

新增的國行元件取自 MYRON 官方 `OS3.0.308.0.WPMCNXM` OTA。ThemeManager、VoiceTrigger 與小愛預設助理 overlay 走 systemless 原始路徑；語音引擎、相簿、編輯器與錄音機則由開機服務安裝到正常 `/data/app`，讓 Android 自己處理 native libraries，不建立額外 bind mount。

### 語音喚醒、語音引擎與預設助理

`com.miui.voicetrigger` 提供小愛的原生語音喚醒、聲紋訓練與設定入口。MYRON 的 Sound Trigger HAL 正常存在；模組只補回國行 APK，不偽造硬體 feature，也不加入推薦內容、`VoiceAssistProxy` 或其他小愛資訊流元件。

主小愛使用 308 `7.12.2.0318`（`507012002`），小愛視覺使用 308 `5.12.4.20`（`540120420`），VoiceTrigger 與語音引擎也同步 308。AI 通話服務的 304／308 版本同為 `6.0.3`（`2535`），因此保留 xiaomi.eu 現有、帶相容語系的 APK，不做沒有版本收益的替換。

308 的問題不是缺少 `VoiceAssistProxy` 或其他 companion，而是 `CoreAliveManager.registerAlive()` 原本只會從小愛的 `AssistInteractionService.onReady()` 呼叫。Google 維持預設助理時，Android 不會啟動小愛的 VoiceInteractionService，冷開機後也就沒有人綁定官方 `VoiceTriggerService`。

`com.xiaomi.mibrain.speech` 是小米的 ASR／TTS 引擎。它在官方 ROM 原本就是可移除 data-app，因此本模組也用正常 `pm install` 安裝，不改簽、不提高成 system App；Google 語音服務仍可保留與選用。

`VoiceAssistAndroidOverlay` 只把 framework 的 `config_defaultAssistant` 預設值設為 `com.miui.voiceassist`。它不移除其他 `VoiceInteractionService`，也不覆寫使用者已明確選好的助理。實機同時可解析 Google、小愛與 ChatGPT；目前已選定的 Google 助理會維持原值，之後仍可在系統設定自由切換。

v1.0.6 搭配 Jason 自用 HyperCeiler：在 `com.miui.voiceassist` scope 由 308 自己的 BootupReceiver 觸發官方 `CoreAliveManager` 註冊，讓服務由 `com.miui.voiceassist` 綁定，不用 root／shell 強啟、不切換預設助理、不清 App data，也不重錄聲紋；`com.miui.voicetrigger` scope 則把同一 session 的重複 `onResourcesAvailable` restart 做成冪等操作，避免已啟動 model 再次呼叫 `startRecognition()` 產生 `-38`。

MYRON 實機已確認 Google 維持預設助理時，小愛與 Hey Google 兩個 SoundTrigger model 可以同時保持 `ACTIVE`，兩邊都能收到真正的 `RECOGNITION` event。模組本身不再從 `service.sh` 直接啟動 VoiceTrigger；生命週期交回 308 的官方 CoreAlive 鏈。

### AI 通話

AI 通話不是另一顆獨立 APK；它就在 `com.xiaomi.aiasst.service` 裡。模組沿用與目前 xiaomi.eu ROM 相容、帶繁中資源的同版本 APK，不以小米應用商店的較新版本覆蓋 ROM，避免 InCallUI、shared UID 或簽章相容問題。

電話 App 的原生 AI 通話項目會向 `com.xiaomi.aiasst.service.aicall.provider` 查詢可用狀態。本模組不偽裝 `ro.product.mod_device`；`com.xiaomi.aiasst.service` 也列入 Taplus 排除清單，不套用任何 Zygisk 欄位翻轉。AI 通話由小米官方 cloud-control 的機型規則、帳號、網路、權限與使用者開關決定可用性。`com.android.contacts`、`com.android.phone` 同樣保留在排除清單。

重新開機後，可在 KernelSU／Magisk／APatch 的本模組頁面按「執行」，叫出小米官方 AI 通話設定完成首次啟用。完成後由 Provider 把原生項目提供給電話 App，不使用假 Launcher APK 或修改 Contacts。第一次啟用仍會顯示小米的使用者協議、隱私政策與系統權限頁；模組不會自動替使用者同意。

### 國行 App 簽章與版本

MYRON xiaomi.eu 內建四個國際版 App 的簽章 SHA-256 是 `f87bd41b…`，這組國行 APK 是小米簽章 `c9009d01…`。本機需要 CorePatch 在 LSPosed 的 `system` scope 啟用下列相容功能：

- 允許不同簽章取代現有 App
- 允許 shared UID 簽章不同（ThemeManager 使用 `android.uid.theme`）
- 允許降版

開機服務按 versionCode 確認相簿、編輯器與錄音機；版本不符時才執行 `pm install -r -d -g`，結果寫在模組目錄的 `cn_media_install.log`。ThemeManager 因 shared UID 與重複 permission 無法安全改走 `/data/app`，仍由 systemless overlay 提供，因此 MYRON 需要 CorePatch。

官方 `MiuiThemeManagerCnOverlay.apk` 由 Package Manager 安裝，再交給 OverlayManager 啟用；不嘗試新增 `/product/overlay` 掛載。

MYRON `OS3.0.308.0.WPMCNXM` 實機比對：

| App | 模組國行版 | ROM 原版 | 變化 |
| --- | --- | --- | --- |
| Gallery | `5.0.5.7-0508-R` | `4.3.1.16-global` | 升級 |
| MediaEditor | `2.3.0.8.3` | `2.4.0.4.3-global` | 降版 |
| SoundRecorder | `7.8.9.3-bc34f3e22` | `7.8.9.9-643c0d7ef` | 降版 |
| ThemeManager | `10.8.0.0` | `10.8.7.6` | 降版 |

安裝器會清掉 Package Manager cache，但不會主動刪除這四個 App 的 user data。若降版後某個 App 因舊資料庫閃退，再只清該 App 的資料，不需要一次清四個。

### 沒有小米錢包

`com.mipay.wallet` 已從 repo 與 ZIP 移除。智慧卡卡包、門卡、交通卡、車鑰匙與基礎 NFC 元件仍在；銀行卡、Mi Pay 錢包入口、雙擊電源開啟 Mi Pay，以及部分依賴錢包的儲值流程不可用。

這不是隱藏圖示或停用 Activity。模組裡沒有小米錢包 APK。

## 這版怎麼決定要加什麼

原則不是把國行 ROM 缺少的 APK 全部補回，而是只加台灣實際用得到、能走系統原生入口、且不需要為它改全域地區的功能。

- 簡訊、日曆、筆記、計算機、掃描器、指南針與天氣已經在 ROM 裡，不用換成另一份國行 APK。
- 支付寶與米家是正常 user App，照原本更新管道使用，不綁進 root 模組。
- 小米遙控器可由 Google Play 安裝，不綁進模組；也不為它改系統 package 或地區。
- 尋找裝置後端 `com.xiaomi.finddevice` 與小米雲服務已存在；國行 `com.miui.findmy` 只是額外入口，而且 Activity 沒有 label，會重現 Launcher 顯示 class 名稱的問題，因此不加。
- 換機、CarWith、數位車鑰匙只在實際有換機或相容車輛時才有價值，不做成每次開機都載入的固定 payload。
- 內容中心、中國瀏覽器、閱讀器、影片、輸入法、清理、黃頁、商店、遊戲中心、廣告與推薦服務明確不加。

## Taplus 長按修復

這版 xiaomi.eu 的 `miui.contentcatcher.InterceptorProxy.create(Activity)` 會在 `miui.os.Build.IS_INTERNATIONAL_BUILD` 為 `true` 時直接回傳 `null`。傳送門設定看起來正常，App 內卻根本沒有建立長按攔截器。

模組內的 arm64 Zygisk hook 會在 App process 啟動後，把該 process 的 `IS_INTERNATIONAL_BUILD` 改為 `false`。ThemeManager 另在 Application 建立完成後，把 App 內的 API region cache 設為 `CN`；啟動資源解析與全域 `ro.miui.region` 仍維持 `TW`，避免 `networkSecurityConfig` 資源錯配。

Google Wallet 與 Google Play services 會強制走 denylist unmount，並卸載本模組 library。這兩個 process 看不到模組 mount，也不執行 Taplus flip。

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
| `com.xiaomi.aiasst.service` | AI 通話只走官方 cloud-control，不套用 Taplus 或其他國際版欄位翻轉 |
| `com.google.android.apps.walletnfcrel` | 支付 App：強制 denylist unmount 並卸載本模組 library |
| `com.google.android.gms` | Play services 全部子程序：強制 denylist unmount 並卸載本模組 library |

排除規則同時涵蓋 `package` 與 `package:suffix` 子程序。修改清單後不用重新刷模組，但必須重新啟動目標 App process；已經執行中的 process 不會自動還原欄位值。

標準 Android `TextView` 已驗證可以觸發 Taplus。YouTube Litho 這類虛擬文字節點不在 MIUI 原生擷取器的可見範圍內，改手勢模式或 flip 國際版旗標都無法補上。

## App 語系

開機服務會設定個別 App 語系，不會改系統語言：

- 小愛語音、語音喚醒、小米語音引擎、小愛視覺、小愛服務／AI 通話：`zh-CN`
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
- 可用的 systemless 掛載能力；KernelSU 可使用 Hybrid Mount 等掛載元模組
- Zygisk Next；沒有它時 App payload 仍可掛載，但 Taplus 國際版修復不會生效
- MYRON 上需 CorePatch，並在 LSPosed 對 `system` 啟用簽章、shared UID 與降版相容開關
- 雙語音喚醒需 Jason 自用 HyperCeiler，LSPosed scope 勾選 `com.miui.voiceassist` 與 `com.miui.voicetrigger`
- arm64 裝置

不要和原版 `HyperOS3EULocalization` 同時啟用，兩者會掛載相同路徑。安裝器偵測到原版仍啟用時會中止。

若裝置上還有獨立的 `taplus_intl_fix`，請移除舊模組。本模組已經內建相同 hook，沒有必要讓每個 App process 載入兩次。

## 安裝

1. 從 root 管理器安裝模組 ZIP。
2. 重新開機。
3. 確認小愛、語音喚醒、語音引擎、傳送門、智慧卡與四個國行 App 都已被系統載入。

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

輸出位於 `dist/`。建置腳本會檢查十一個 systemless payload、五個正常安裝 payload、arm64 Zygisk binary 與排除清單；只要出現小米錢包、舊三 App systemless 路徑或 `post-fs-data.sh`，建置就會中止。

重新編譯 Zygisk hook：

```sh
NDK_BUILD=/path/to/ndk-build ./scripts/build_zygisk.sh
```

## 上游與授權

這個 repo 保留 GitHub fork 關係，主要修改集中在固定 payload、Taplus／Theme region Zygisk hook、支付 App 安全排除、App 語系與無互動安裝流程。語音喚醒、語音引擎、預設助理 overlay 與四個國行媒體 App 取自 MYRON 官方 308 OTA。

專案沿用上游的 GPL-3.0 授權。原始工作可追溯至 LSHFGJ/HyperOS3EULocalization 與 MinaMichita/MiuiEULocalizationToolsBox。
