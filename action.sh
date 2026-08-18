#!/system/bin/sh

# Root manager 的模組「執行」按鈕：叫出 AI 通話的官方設定入口。
# 首次啟用後，正常入口會由 AI 通話 Provider 提供給電話 App。
# 這裡不自動接受隱私條款，也不替使用者變更 AI 通話開關。
AI_CALL_PACKAGE=com.xiaomi.aiasst.service
AI_CALL_SETTINGS_ACTION=com.xiaomi.aiasst.service.aicall.settings

if ! pm path "$AI_CALL_PACKAGE" >/dev/null 2>&1; then
    echo "找不到 AI 通話（$AI_CALL_PACKAGE），請先重新開機讓模組 payload 載入。"
    exit 1
fi

if ! am start --user 0 -W -a "$AI_CALL_SETTINGS_ACTION" >/dev/null 2>&1; then
    echo "AI 通話設定入口無法啟動。"
    exit 1
fi

echo "已開啟 AI 通話設定。"
