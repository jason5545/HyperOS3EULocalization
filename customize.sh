##########################################################################################
# HyperOS 3 EU XiaoAI / Portal / Mi Pay - Magisk/KernelSU/APatch installer
##########################################################################################

SKIPUNZIP=1
ASH_STANDALONE=1

ui_print ""
ui_print "[HyperOS 3 EU 小愛・語音喚醒・傳送門・Mi Pay]"
ui_print "- 固定安裝小愛、語音喚醒、AI 通話、傳送門、智慧卡與必要支付服務"
ui_print "- 國行系統桌面走 systemless，zygisk 保留 Google 負一屏資料來源"
ui_print "- 相簿走正常 App 安裝；編輯器、錄音機（priv-app）、主題商店走 systemless"
ui_print "- 不安裝小米錢包 App；銀行卡與部分儲值入口不可用"
ui_print "- 內建 Taplus 國際版 Zygisk 修復，不安裝 Focus overlay"
ui_print "- 小愛固定簡中，其餘新增 App 固定繁中（台灣）"
ui_print ""

ui_print "- 解壓縮模組"
unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODPATH" >&2

chmod -R 0755 "$MODPATH/tools"
. "$MODPATH/tools/unity_install.sh"

ui_print "- 清理安裝器檔案"
rm -rf \
    "$MODPATH/customize.sh" \
    "$MODPATH/build.sh" \
    "$MODPATH/README.md" \
    "$MODPATH/LICENSE" \
    "$MODPATH/tools" 2>/dev/null

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755

ui_print ""
ui_print "安裝完成，請重新開機。"
ui_print "KernelSU 使用者需先啟用可用的 systemless 掛載元模組。"
ui_print "Taplus 長按修復另需 Zygisk Next 正常啟用。"
ui_print "語音引擎與國行相簿由開機服務安裝；ThemeManager 仍需要 CorePatch 相容。"
ui_print "重新開機後可用模組的「執行」按鈕完成 AI 通話首次啟用。"
