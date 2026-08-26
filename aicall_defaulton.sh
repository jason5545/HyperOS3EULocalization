#!/system/bin/sh
# aicall_defaulton.sh — AI 通話入口預設開啟（一次性，開機由 service.sh 觸發）
#
# 2026-08-26 反編譯＋實機定因（詳見 AGENTS.md「AI 通話入口 gate 與預設開啟」）：
# 撥號盤 ⋮ 選單與通話中按鈕由 Contacts 向
# content://com.xiaomi.aiasst.service.aicall.provider 查 GET_AICALL_AVAILABLE
# 回傳 status 決定；status 直接等於 MIUIAiasstService shared_prefs
# setting.xml 的 incallctrlbutton（缺省 false → status 0 → 入口隱藏）。
# 恢復原廠後該 key 不存在；設定頁開關在缺懸浮窗權限時只彈授權框、
# 不落盤（InCallCtrlSettingFragment 的 q0() 分支），故在此補預設 true。
# key 已存在（使用者明確開／關）一律不動。
#
# 寫檔鐵律（三條都是 2026-08-26 實機踩過的坑）：
# 1. 寫入走「暫存檔 → 補 owner/mode → mv」：rename 原子替換，避免
#    cat > 的半截檔案；絕對不可 sed -i（toybox sed -i 建新 inode，
#    owner 變 root:root，App 反而讀不到自己的 prefs）。
# 2. 寫完若 App 在跑，kill 進程讓它重讀（SharedPreferences 不看外部
#    檔案變更）。絕對不可 am force-stop：force-stop 會把套件標成
#    stopped，之後 resolveActivity 解析不到 aicalllog_detail、
#    AI 通話入口整個消失（實測症狀：pref 已 true 但 ⋮ 選單仍沒有）。
# 3. setting.xml 不存在才建新檔，owner 比照 shared_prefs 目錄
#    （root 在該目錄建檔，SELinux context 自動是 system_app_data_file）。

AICALL_PKG=${AICALL_PKG:-com.xiaomi.aiasst.service}
USER_DIR=${AICALL_USER_DIR:-/data/user/0}
LOG=${AICALL_DEFAULTON_LOG:-/dev/null}

PREFS_DIR="$USER_DIR/$AICALL_PKG/shared_prefs"
PREFS_FILE="$PREFS_DIR/setting.xml"
PREF_KEY=incallctrlbutton
PREF_LINE="    <boolean name=\"$PREF_KEY\" value=\"true\" />"

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

[ -d "$USER_DIR/$AICALL_PKG" ] || { log "SKIP: $AICALL_PKG data dir 不存在"; exit 0; }

# 已表態（true 或 false）就不動：「預設開啟」只補缺省，不覆寫使用者選擇
if grep -q "name=\"$PREF_KEY\"" "$PREFS_FILE" 2>/dev/null; then
    exit 0
fi

# 懸浮窗權限：mode=default（從未表態）才補 allow；deny/ignore 是使用者
# 明確拒絕，不覆寫。設定頁開關落盤與通話中浮動控制都需要它。
case "$(appops get "$AICALL_PKG" SYSTEM_ALERT_WINDOW 2>/dev/null | head -n 1)" in
*": default"*)
    if appops set "$AICALL_PKG" SYSTEM_ALERT_WINDOW allow 2>/dev/null; then
        log "OPS: SYSTEM_ALERT_WINDOW default -> allow"
    fi
    ;;
esac

TMP_FILE="$PREFS_FILE.jrc"
if [ -f "$PREFS_FILE" ]; then
    awk -v ins="$PREF_LINE" '{ if (!done && $0 ~ /<\/map>/) { print ins; done=1 } print }' \
        "$PREFS_FILE" > "$TMP_FILE"
else
    printf "%s\n<map>\n%s\n</map>\n" \
        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>" "$PREF_LINE" \
        > "$TMP_FILE"
fi
chown "$(ls -ld "$PREFS_DIR" | awk '{print $3":"$4}')" "$TMP_FILE" 2>/dev/null
chmod 0660 "$TMP_FILE"
mv "$TMP_FILE" "$PREFS_FILE"

# App 在跑就殺掉重讀（kill 不標 stopped；見檔頭鐵律 2）
AICALL_PID=$(pidof "$AICALL_PKG" 2>/dev/null)
if [ -n "$AICALL_PID" ]; then
    kill "$AICALL_PID" 2>/dev/null
    log "KILL: $AICALL_PKG($AICALL_PID) 重啟以重讀 prefs"
fi

if grep -q "name=\"$PREF_KEY\" value=\"true\"" "$PREFS_FILE" 2>/dev/null; then
    log "OK: $PREF_KEY=true（AI 通話入口預設開啟）"
else
    log "FAIL: 寫入後 $PREFS_FILE 仍無 $PREF_KEY"
fi
