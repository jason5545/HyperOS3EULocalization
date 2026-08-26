#!/system/bin/sh

MODDIR=${0%/*}
SYSTEM_VERSION="$(getprop ro.system.build.version.incremental)"
SYSTEM_VERSION=${SYSTEM_VERSION:-unknown}
VERSION_DIR="$MODDIR/system/etc/localization/SystemVersion"

ROM_CHANGED=0
if [ ! -f "$VERSION_DIR/$SYSTEM_VERSION" ]; then
    ROM_CHANGED=1
    rm -rf /data/system/package_cache/*
    rm -rf "$VERSION_DIR"
    mkdir -p "$VERSION_DIR"
    touch "$VERSION_DIR/$SYSTEM_VERSION"
fi

# 模組所有 payload APK 的 package/versionCode/路徑清單（build.sh 以 aapt2
# 生成）：data-app 期望版本、systemless 登錄稽核都以此為唯一真相來源。
PAYLOAD_VERSIONS="$MODDIR/payload_versions.txt"
payload_vc() {
    [ -f "$PAYLOAD_VERSIONS" ] || return 1
    sed -n "s/^$1 \([0-9][0-9]*\) .*/\1/p" "$PAYLOAD_VERSIONS" | head -n 1
}

# 等系統服務與 user 0 ready，再套用每個 App 的語系。
# 小愛、語音喚醒、語音引擎與 AI 通話使用簡中；其餘新增 App 使用繁中（台灣）。
BOOT_WAIT=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$BOOT_WAIT" -lt 120 ]; do
    sleep 2
    BOOT_WAIT=$((BOOT_WAIT + 1))
done

# 語音引擎、Gallery 走正常 /data/app 安裝，讓 Android
# 自己解出 native libraries；不要建立額外 bind mount，避免支付 App 看見
# KernelSU 模組掛載。ThemeManager 因 shared UID/重複 permission 保留 systemless。
# SoundRecorder 自 v1.0.11 改為 systemless priv-app：搭配模組自帶的
# privapp 授權 XML 才有 WRITE_MEDIA_STORAGE／CAPTURE_AUDIO_OUTPUT。
# MediaEditor 自 v1.0.12 改為 systemless：EU 308 底包內建 2.4.0.4.3-global
# （vc 204990043）高於 CN 版（203990083），data-app 會被當過期更新丟棄。
DATA_PAYLOAD_DIR="$MODDIR/payload"
DATA_INSTALL_TMP=/data/local/tmp/jrc_data_app_install
DATA_INSTALL_LOG="$MODDIR/data_app_install.log"

installed_version_code() {
    dumpsys package "$1" 2>/dev/null \
        | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' \
        | head -n 1
}

ensure_data_app() {
    PACKAGE_NAME="$1"
    RELATIVE_APK="$2"
    EXPECTED_VERSION="$(payload_vc "$PACKAGE_NAME")"
    APK_PATH="$DATA_PAYLOAD_DIR/$RELATIVE_APK"
    APK_NAME="${RELATIVE_APK##*/}"

    if [ -z "$EXPECTED_VERSION" ]; then
        echo "NO-MANIFEST: $PACKAGE_NAME（payload_versions.txt 缺條目，跳過）" >> "$DATA_INSTALL_LOG"
        return
    fi
    if [ "$(installed_version_code "$PACKAGE_NAME")" = "$EXPECTED_VERSION" ]; then
        return
    fi
    if [ ! -f "$APK_PATH" ]; then
        echo "MISSING: $APK_PATH" >> "$DATA_INSTALL_LOG"
        return
    fi

    mkdir -p "$DATA_INSTALL_TMP"
    TMP_APK="$DATA_INSTALL_TMP/$APK_NAME"
    cp "$APK_PATH" "$TMP_APK"
    # 開機風暴裡 AMS 可能整批拒絕 binder transaction（2026-08-26 實測
    # "cmd: Failure calling service package: Failed transaction (2147483646)"
    # 讓 Gallery 整輪沒裝到）。同雙喚醒的 redeliver 模式：同輪短退避再試，
    # 成功與否以輸出含 Success 為準。
    INSTALL_ATTEMPT=0
    while [ "$INSTALL_ATTEMPT" -lt 3 ]; do
        INSTALL_ATTEMPT=$((INSTALL_ATTEMPT + 1))
        INSTALL_OUT=$(pm install -r -d -g "$TMP_APK" 2>&1)
        echo "$INSTALL_OUT" >> "$DATA_INSTALL_LOG"
        case "$INSTALL_OUT" in
        *Success*)
            echo "SUCCESS: $PACKAGE_NAME -> $EXPECTED_VERSION" >> "$DATA_INSTALL_LOG"
            break
            ;;
        esac
        echo "RETRY ${INSTALL_ATTEMPT}: $PACKAGE_NAME（$(echo "$INSTALL_OUT" | tail -n 1)）" >> "$DATA_INSTALL_LOG"
        [ "$INSTALL_ATTEMPT" -lt 3 ] && sleep 2
    done
    case "$INSTALL_OUT" in
    *Success*) ;;
    *) echo "FAILED: $PACKAGE_NAME -> $EXPECTED_VERSION" >> "$DATA_INSTALL_LOG" ;;
    esac
    rm -f "$TMP_APK"
}

echo "=== data app ensure: $(date) ===" > "$DATA_INSTALL_LOG"
ensure_data_app com.xiaomi.mibrain.speech xiaoai/MIUIXiaoAiSpeechEngine.apk
ensure_data_app com.miui.gallery cn-media/MiuiGallery.apk

# --- PM 系統 App 登錄稽核與 data shadow 自愈 ---------------------------------
# 2026-08-26 ReSukiSU 遷移實測定因（詳見 AGENTS.md「PM 嚴格升級保留」）：
# PackageManager 只在「掃到的 versionCode 嚴格大於已登錄值」時才重寫系統
# App 登錄。root 方案遷移／掛載失敗的開機會讓 EU 底包（vc 較高或同號異
# build）卡進登錄；之後掛回 CN payload，PM 永久保留 EU manifest＋CN 程式碼
# → SoundRecorder/MediaEditor 的 installProvider 整批 ClassNotFound 直接 FC，
# MiuiHome 則是登錄 2545／實跑 2529。packages.xml 已是 ABX binary，shell
# 無法手術；本段每輪開機稽核並在 vc 允許時（CN ≥ 已登錄值）以 data shadow
# 立即接手 CN manifest，其餘只記警告（待 payload 升版／GetApps CN 更新／
# OTA 全量重掃自然收斂）。
if [ -f "$PAYLOAD_VERSIONS" ]; then
    # ROM 變更（mIsUpgrade 全量重掃）：先卸掉殘留 data shadow，讓重掃後的
    # systemless 登錄接手；shadow 只在登錄卡 EU 期間有存在價值。
    if [ "$ROM_CHANGED" = "1" ]; then
        for SHADOW_PKG in com.android.soundrecorder com.miui.mediaeditor; do
            case "$(pm path "$SHADOW_PKG" 2>/dev/null | head -n 1)" in
            package:/data/app/*)
                if pm uninstall "$SHADOW_PKG" >> "$DATA_INSTALL_LOG" 2>&1; then
                    echo "SHADOW-CLEAR(ROM change): $SHADOW_PKG" >> "$DATA_INSTALL_LOG"
                fi
                ;;
            esac
        done
    fi
    while read -r SPKG SVC SPATH; do
        case "$SPKG" in ''|\#*) continue ;; esac
        case "$SPATH" in payload/*) continue ;; esac  # data payload 由上方 ensure 處理
        SREG=$(installed_version_code "$SPKG")
        [ "$SREG" = "$SVC" ] && continue               # 登錄正確（含 shadow 在役）
        [ -z "$SREG" ] && continue                     # 未登錄：PM 掃到自然會登錄
        case "$(pm path "$SPKG" 2>/dev/null | head -n 1)" in
        package:/data/app/*)
            echo "AUDIT: $SPKG data shadow 在役但版本不符（registered=$SREG shipped=$SVC）" >> "$DATA_INSTALL_LOG"
            ;;
        *)
            if [ "$SVC" -ge "$SREG" ] 2>/dev/null && [ "${SPATH#*priv-app}" = "$SPATH" ]; then
                # 非 priv-app 且 CN vc ≥ 卡住的 EU 登錄：裝 data shadow，
                # CN manifest 立即生效（priv-app 走 data 會丟 privileged
                # grants，只能記警告）。
                SHADOW_NAME="${SPATH##*/}"
                mkdir -p "$DATA_INSTALL_TMP"
                cp "$MODDIR/$SPATH" "$DATA_INSTALL_TMP/$SHADOW_NAME"
                if pm install -r -d -g "$DATA_INSTALL_TMP/$SHADOW_NAME" >> "$DATA_INSTALL_LOG" 2>&1; then
                    echo "SHADOWED: $SPKG $SREG -> $SVC（systemless 登錄卡住，data shadow 接手）" >> "$DATA_INSTALL_LOG"
                fi
                rm -f "$DATA_INSTALL_TMP/$SHADOW_NAME"
            else
                echo "STALE: $SPKG 登錄=$SREG 模組=$SVC（PM 嚴格升級保留 EU manifest；待 payload 升版／GetApps CN 更新／OTA 收斂）" >> "$DATA_INSTALL_LOG"
            fi
            ;;
        esac
    done < "$PAYLOAD_VERSIONS"
fi

# 官方 308 static overlay 在 hybrid_mount 環境可能無法新增到 /product/overlay；
# 改由 Package Manager 安裝並讓 OverlayManager 啟用。
THEME_OVERLAY_PACKAGE=com.android.thememanager.customthemeconfig.config.overlay
THEME_OVERLAY_APK="$DATA_PAYLOAD_DIR/cn-media/MiuiThemeManagerCnOverlay.apk"
if ! pm path "$THEME_OVERLAY_PACKAGE" >/dev/null 2>&1 && [ -f "$THEME_OVERLAY_APK" ]; then
    mkdir -p "$DATA_INSTALL_TMP"
    cp "$THEME_OVERLAY_APK" "$DATA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk"
    pm install -r -d -g "$DATA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk" \
        >> "$DATA_INSTALL_LOG" 2>&1
    rm -f "$DATA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk"
fi
cmd overlay enable --user 0 "$THEME_OVERLAY_PACKAGE" >/dev/null 2>&1
rm -rf "$DATA_INSTALL_TMP"

set_app_locale() {
    PACKAGE_NAME="$1"
    LOCALE_CONFIG="$2"
    APP_LOCALE="$3"
    cmd locale set-app-localeconfig "$PACKAGE_NAME" --user 0 --locales "$LOCALE_CONFIG" >/dev/null 2>&1
    cmd locale set-app-locales "$PACKAGE_NAME" --user 0 --locales "$APP_LOCALE" >/dev/null 2>&1
}

app_locale_is() {
    [ "$(cmd locale get-app-locales "$1" --user 0 2>/dev/null \
        | sed -n 's/.*are \[\(.*\)\]/\1/p')" = "$2" ]
}

ensure_app_locale() {
    app_locale_is "$1" "$3" && return
    LOCALE_FAILED=1
    set_app_locale "$1" "$2" "$3"
}

# /data/system_ce 要解鎖後才掛載，boot_completed 可能早於解鎖：直接寫會吃
# LocaleManagerService IOException，該次開機的語系設定整個沒生效
# （2026-08-21 07:46 實測，開機後 30 秒內前兩個 App 寫入失敗）。
# 先等 ce ready（有上限，超時仍往下走、靠驗證重試收尾），
# 再逐個「驗證不符才寫入」，失敗的下一輪重試。
LOCALE_CE_WAIT=0
while [ ! -d /data/system_ce/0 ] && [ "$LOCALE_CE_WAIT" -lt 30 ]; do
    sleep 2
    LOCALE_CE_WAIT=$((LOCALE_CE_WAIT + 1))
done

LOCALE_ROUND=0
while [ "$LOCALE_ROUND" -lt 3 ]; do
    LOCALE_FAILED=0
    ensure_app_locale com.miui.voiceassist zh-CN,zh-TW zh-CN
    ensure_app_locale com.miui.voicetrigger zh-CN,zh-TW zh-CN
    ensure_app_locale com.xiaomi.mibrain.speech zh-CN,zh-TW zh-CN
    ensure_app_locale com.xiaomi.aiasst.vision zh-CN,zh-TW zh-CN
    ensure_app_locale com.xiaomi.aiasst.service zh-CN,zh-TW zh-CN

    ensure_app_locale com.miui.contentextension zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.nextpay zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.tsmclient zh-TW,zh-CN zh-TW
    ensure_app_locale com.unionpay.tsmservice.mi zh-TW,zh-CN zh-TW
    ensure_app_locale com.xiaomi.payment zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.gallery zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.mediaeditor zh-TW,zh-CN zh-TW
    ensure_app_locale com.android.soundrecorder zh-TW,zh-CN zh-TW
    ensure_app_locale com.android.thememanager zh-TW,zh-CN zh-TW
    [ "$LOCALE_FAILED" = "0" ] && break
    LOCALE_ROUND=$((LOCALE_ROUND + 1))
    sleep 5
done

# --- 長按電源 3 秒電源選單保底 ----------------------------------------------
# 2026-08-26 定因（詳見 AGENTS.md「長按電源 3 秒電源選單」）：xiaomi.eu 底層
# IS_INTERNATIONAL_BUILD=true 但 IS_GLOBAL_BUILD=false，
# MiuiShortcutTriggerHelper.shouldShowPowerPanel() 走 CN 分支恆 false，初次
# 計算把 should_launch_global_power_panel 寫成 0 → power_button_very_long_press=0
# → 長按電源 3 秒的電源選單整個消失（小愛那個圓圈只是倒數動畫，選單本體是
# framework 的 very-long-press → GLOBAL_ACTIONS）。framework 對
# should_launch_global_power_panel 有 observer，補回 1 會立刻重算寫回
# power_button_very_long_press=1；global_power_guide=0 是給開機早期
# mShouldShowPowerPanel==-1 的初始路徑兜底（該路徑讀這個 v1 key，
# 值缺省當 1 會把選單算成關閉）。
[ "$(settings get system should_launch_global_power_panel)" = "1" ] || \
    settings put system should_launch_global_power_panel 1
[ "$(settings get system global_power_guide)" = "0" ] || \
    settings put system global_power_guide 0

# --- AI 通話入口預設開啟 -----------------------------------------------------
# 2026-08-26 定因（詳見 AGENTS.md「AI-call entry gate & default-on」與
# aicall_defaulton.sh 檔頭）：撥號盤 ⋮ 選單／通話中的 AI 通話入口由
# MIUIAiasstService provider 的 GET_AICALL_AVAILABLE status 決定，真正的
# gate 是 shared_prefs setting.xml 的 aicall_onoff（雲控在 EU 預設 false，
# 恢復原廠後 key 不存在 → 入口整個隱藏）。一次性 worker 把
# aicall_onoff/callscreen_onoff/incallctrlbutton/privacy 四鍵全補 true
# ＋overlay 權限（使用者要求無條件開啟，明確 false 也翻回）。同 dualwake
# 模式：複製到 /data/local/tmp 執行，跑完即退。
AICALL_TMP=/data/local/tmp/jrc_aicall_defaulton.sh
cp "$MODDIR/aicall_defaulton.sh" "$AICALL_TMP"
AICALL_DEFAULTON_LOG="$MODDIR/aicall_defaulton.log" nohup sh "$AICALL_TMP" >/dev/null 2>&1 &

# --- 雙喚醒冷開機保底 -------------------------------------------------------
# 1) 冷開機記憶體高峰時，MIUI 可能在 BootupReceiver 結束後數十毫秒內回收
#    com.miui.voiceassist:voice_trigger，讓 CoreAlive 內部的 bind 來不及執行。
# 2) 2026-08-21 實測定因：開機風暴裡 GSA 的 AoHD 鏈可能卡死不載入模型
#    （實測卡 13 分鐘）；killall audioserver 只會把事件送達鏈打斷（DSP 有
#    偵測、助理不起來），絕對不可 bounce。解法是小愛武裝後盯 GSA 模型，
#    逾寬限未載入就殺 GSA 的 isolated hotword process 讓系統自動重建整條
#    鏈（細節見 dualwake_boot.sh 與 AGENTS.md「Dual-wake boot race」）。
DUALWAKE_LOG="$MODDIR/dualwake_boot.log"
# 只在明確「0」（使用者關閉語音喚醒）時才不跑；null（重設後尚未再設定）
# 也要跑——worker 自己會判斷武裝與註冊狀態，閒置 12 輪即退，且 GSA 開機
# 卡死與小愛是否武裝無關（2026-08-26 ReSukiSU 遷移後此值歸 null，閘門
# 誤殺整個 worker，Hey Google 的卡死也沒人盯）。
if [ "$(settings get global voice_trigger_enabled)" != "0" ]; then
    case "$(settings get secure voice_interaction_service)" in
    com.miui.voiceassist/*)
        : # 小愛已是預設助理：官方 VoiceInteractionService 鏈自己會處理
        ;;
    *)
        # 獨立背景 worker，service.sh 本體立刻結束：避免開機後數分鐘
        # （支付 App 首次啟動的敏感窗口）系統裡常駐一個 cmdline 帶
        # /data/adb/modules 路徑的 root shell。worker 複製到 /data/local/tmp
        # 執行，記錄檔路徑走環境變數，不出現在 cmdline。
        WORKER_TMP=/data/local/tmp/jrc_dualwake_boot.sh
        cp "$MODDIR/dualwake_boot.sh" "$WORKER_TMP"
        DUALWAKE_LOG="$DUALWAKE_LOG" nohup sh "$WORKER_TMP" >/dev/null 2>&1 &
        ;;
    esac
fi

# --- gms/wallet/vending mount namespace 清道夫 ------------------------------
# 2026-08-24 實測定因（細節見 mount_scrub.sh 檔頭）：KSU 的 per-app umount
# 只在 specialize 生效一次；其他模組腳本的 runtime bind（BW_Audio dolby/
# quasar、morphe 修補 APK）會經 shared propagation 滲入還活著的 Google
# 進程，/proc/self/mountinfo 出現 /adb/modules 來源 → DroidGuard 判 root
# （Wallet/GMS「有時跳提示、殺掉重開就好」的成因）。worker 週期性清掉
# 這三族進程 namespace 裡的 /adb/modules 來源掛載，同 dualwake 模式：
# 複製到 /data/local/tmp 執行，不常駐帶模組路徑的 shell。
SCRUB_TMP=/data/local/tmp/jrc_mount_scrub.sh
cp "$MODDIR/mount_scrub.sh" "$SCRUB_TMP"
MOUNT_SCRUB_LOG="$MODDIR/mount_scrub.log" nohup sh "$SCRUB_TMP" >/dev/null 2>&1 &
