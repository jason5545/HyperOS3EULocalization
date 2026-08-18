##########################################################################################
# HyperOS 3 EU XiaoAI / Portal / Mi Pay - Magisk/KernelSU/APatch installer
##########################################################################################

SKIPUNZIP=1
ASH_STANDALONE=1

ui_print ""
ui_print "[HyperOS 3 EU 小愛・傳送門・Mi Pay]"
ui_print "- 固定安裝小愛、傳送門與完整 Mi Pay 鏈路"
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

ui_print ""
ui_print "安裝完成，請重新開機。"
ui_print "KernelSU 使用者需先啟用可用的 systemless 掛載元模組。"
ui_print "Taplus 長按修復另需 Zygisk Next 正常啟用。"
