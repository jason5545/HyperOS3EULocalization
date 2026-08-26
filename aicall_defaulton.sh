#!/system/bin/sh
# aicall_defaulton.sh — AI 通話入口預設開啟（開機由 service.sh 觸發）
#
# 2026-08-26 反編譯＋實機定因（詳見 AGENTS.md「AI-call entry gate & default-on」）：
# 撥號盤 ⋮ 選單與通話中入口由 Contacts 向
# content://com.xiaomi.aiasst.service.aicall.provider 查 GET_AICALL_AVAILABLE，
# 走 p033g2.a.b()：h()=SettingsSp.getAIcallStatus 讀 shared_prefs setting.xml
# 的 aicall_onoff（雲控 ai_call_callscreen 在 EU 不給值 → 預設 false），
# true → status 1，false → 2（隱藏）；e() 模式 → 4、focus mode → 5，
# 只有 {1,3,4} 顯示入口。privacy 未同意（CTA）→ AbstractC0709u.q() 失敗
# → status 6，入口一樣消失。
# 本 worker 每輪把四個 key 全部強制 true（明確 false 也翻回；使用者要求
# 無條件開啟，不做缺省判斷）：
#   aicall_onoff      ⋮ 選單入口總開關（真正的 gate）
#   callscreen_onoff  通話中入口（雲控 ai_call_callscreen_entrance 同樣不給 EU）
#   incallctrlbutton  通話中聲控按鈕（GET_INCALL_VOICE_SETTINGS 路徑）
#   privacy           AI 通話隱私協議（CTA）預先同意（等於模組代為同意）
# 懸浮窗權限也無條件補 allow：設定頁開關在缺權限時只彈授權框、不落盤
# （InCallCtrlSettingFragment 的 q0() 分支），通話中浮動控制也需要它。
#
# 寫檔注意（三條都是 2026-08-26 實機踩過的坑）：
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
KEYS="aicall_onoff callscreen_onoff incallctrlbutton privacy"

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

[ -d "$USER_DIR/$AICALL_PKG" ] || { log "SKIP: $AICALL_PKG data dir 不存在"; exit 0; }

# 懸浮窗權限：無條件 allow（見檔頭）；已 allow 就不重複 set
case "$(appops get "$AICALL_PKG" SYSTEM_ALERT_WINDOW 2>/dev/null | head -n 1)" in
*": allow"*) ;;
*)
    if appops set "$AICALL_PKG" SYSTEM_ALERT_WINDOW allow 2>/dev/null; then
        log "OPS: SYSTEM_ALERT_WINDOW -> allow"
    fi
    ;;
esac

# 四個 key 全已 true 就沒事可做（不寫檔、不殺進程）
ALL_ON=1
for k in $KEYS; do
    grep -q "name=\"$k\" value=\"true\"" "$PREFS_FILE" 2>/dev/null || { ALL_ON=0; break; }
done
[ "$ALL_ON" = 1 ] && exit 0

TMP_FILE="$PREFS_FILE.jrc"
if [ -f "$PREFS_FILE" ]; then
    # 單趟改寫：已存在的 key 行統一翻成 true 行，缺的 key 插在 </map> 前
    awk -v keys="$KEYS" '
        BEGIN { n = split(keys, ka, " "); for (i = 1; i <= n; i++) seen[ka[i]] = 0 }
        {
            for (i = 1; i <= n; i++)
                if (index($0, "name=\"" ka[i] "\"") > 0) {
                    $0 = "    <boolean name=\"" ka[i] "\" value=\"true\" />"
                    seen[ka[i]] = 1
                }
            if (index($0, "</map>") > 0)
                for (i = 1; i <= n; i++)
                    if (!seen[ka[i]]) print "    <boolean name=\"" ka[i] "\" value=\"true\" />"
            print
        }' "$PREFS_FILE" > "$TMP_FILE"
else
    {
        printf '%s\n' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>"
        printf '%s\n' "<map>"
        for k in $KEYS; do printf '    <boolean name="%s" value="true" />\n' "$k"; done
        printf '%s\n' "</map>"
    } > "$TMP_FILE"
fi
chown "$(ls -ld "$PREFS_DIR" | awk '{print $3":"$4}')" "$TMP_FILE" 2>/dev/null
chmod 0660 "$TMP_FILE"
mv "$TMP_FILE" "$PREFS_FILE"

# App 在跑就殺掉重讀（kill 不標 stopped；見檔頭注意 2）
AICALL_PID=$(pidof "$AICALL_PKG" 2>/dev/null)
if [ -n "$AICALL_PID" ]; then
    kill "$AICALL_PID" 2>/dev/null
    log "KILL: $AICALL_PKG($AICALL_PID) 重啟以重讀 prefs"
fi

# 逐 key 驗證
FAILED=""
for k in $KEYS; do
    grep -q "name=\"$k\" value=\"true\"" "$PREFS_FILE" 2>/dev/null || FAILED="$FAILED $k"
done
if [ -z "$FAILED" ]; then
    log "OK: AI 通話四鍵全開（${KEYS}）"
else
    log "FAIL: 寫入後仍缺${FAILED}"
fi
