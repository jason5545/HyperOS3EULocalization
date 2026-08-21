#!/system/bin/sh
# 雙喚醒冷開機保底 worker（由 service.sh 複製到 /data/local/tmp 後以 nohup
# 背景啟動；service.sh 本體立刻結束，不常駐帶模組路徑的 root shell）。
#
# 職責一（舊）：冷開機記憶體高峰時，MIUI 可能在小愛 BootupReceiver 結束後
#   數十毫秒內回收 com.miui.voiceassist:voice_trigger，CoreAlive 內部的
#   bind 來不及執行。未武裝時重送開機廣播給小愛自己的 BootupReceiver
#   （不直接啟動 VoiceTrigger 服務、不切預設助理；綁定仍由官方 CoreAlive
#   鏈完成，caller 是小愛）。
#
# 職責二（2026-08-21 重寫，取代 08-20 的 audioserver bounce 版）：
#   GSA（Google）的 AlwaysOnHotwordDetector 鏈（system_server
#   SoundTriggerHelper ↔ isolated hotword process ↔ 第二階段音訊驗證）
#   在開機風暴裡可能只初始化一半就卡住——實測 GSA 21:36:10 attach 後
#   卡住 13 分鐘才自我重建（21:49:02），卡住期間 Hey Google 全死。
#
#   08-20 的 bounce 策略已證明有害，絕對不可 bounce audioserver：
#   無序拆 HAL 只重建 middleware session，GSA 這條鏈卻留下半舊狀態
#   ——DSP 仍在偵測（middleware 有 RECOGNITION status 0），事件卻送不到
#   GSA，助理不起來（實測拖了七小時，期間小愛完全正常）。有效的復原
#   動作是重建整條 AoHD 鏈：使用者在「預設系統應用程式 → 小幫手與語音
#   助理」來回切換時，系統正是這麼做的（06:56:40 isolated process 被
#   回收 → 5 秒內重綁、重掛 session、重載模型 → 雙邊恢復）。
#
#   對策：小愛武裝後盯 GSA 的模型；超過 DUALWAKE_GSA_GRACE 輪仍未載入，
#   就殺 GSA 的 isolated hotword process（名含
#   googlequicksearchbox:trusted_disable_art_image），讓 system_server
#   自動重綁重建（07:18 活體驗證：5 秒內新 process、新 session、新模型
#   就位，小愛 session 完全不受影響）。最多重建 DUALWAKE_GSA_MAX_FIXES
#   次。殺的是一般 App process，不碰音訊服務，通話中也不用避讓。
#
# 環境變數（主要給主機端測試用）：
#   DUALWAKE_INTERVAL      每輪間隔秒數（預設 15）
#   DUALWAKE_MAX_TRIES     最多幾輪（預設 12）
#   DUALWAKE_GSA_GRACE     小愛武裝後給 GSA 自己載入的輪數（預設 4）
#   DUALWAKE_GSA_MAX_FIXES 最多重建 GSA AoHD 鏈幾次（預設 2）
#   DUALWAKE_KILL_BIN      kill 指令路徑（預設 kill；主機端測試用 stub 取代，
#                          因為 POSIX sh 的 kill 是 builtin,PATH stub 蓋不掉）
#   DUALWAKE_LOG           記錄檔路徑（預設 /dev/null）

INTERVAL="${DUALWAKE_INTERVAL:-15}"
MAX_TRIES="${DUALWAKE_MAX_TRIES:-12}"
GSA_GRACE="${DUALWAKE_GSA_GRACE:-4}"
GSA_MAX_FIXES="${DUALWAKE_GSA_MAX_FIXES:-2}"
KILL_BIN="${DUALWAKE_KILL_BIN:-kill}"
LOG="${DUALWAKE_LOG:-/dev/null}"

dump_st() {
    dumpsys soundtrigger_middleware 2>/dev/null
}

xiaoai_armed() {
    # 小愛的 SoundTrigger 模型載入後，active model 清單帶 keyphrase 文字
    # （只有最後的 active model 清單有 text:，detached session 只有事件）
    dump_st | grep -q 'xiaoaitongxue'
}

gsa_armed() {
    # GSA 模型已載入：active model 描述帶 keyphrase 文字（text: X Google）
    # 或固定的 vendorUuid 7038ddc8-30f2-…（uuid 每次重建都會變）
    dump_st | grep -qE 'text: X Google|vendorUuid: 7038ddc8-30f2'
}

gsa_hotword_pids() {
    # isolated hotword process；[e] 技巧避免 pgrep 比到自己的 cmdline
    pgrep -f 'googlequicksearchbox:trusted_disable_art_imag[e]'
}

redeliver_bootup() {
    am broadcast -a android.intent.action.BOOT_COMPLETED \
        -n com.miui.voiceassist/com.xiaomi.voiceassistant.voiceTrigger.adapter.BootupReceiver \
        >> "$LOG" 2>&1
}

try=0
armed_rounds=0
fixes=0
while [ "$try" -lt "$MAX_TRIES" ]; do
    if ! xiaoai_armed; then
        # 未武裝：立刻重送開機廣播喚起官方鏈（BOOT_COMPLETED 在開機風暴的
        # 廣播佇列裡可能等 90 秒以上，這裡主動戳；AMS 忙不過來會失敗，下一輪
        # 再試），再等 INTERVAL 進入下一輪。
        try=$((try + 1))
        echo "try $try: VoiceTrigger 未武裝，重送 BootupReceiver $(date)" >> "$LOG"
        redeliver_bootup
        sleep "$INTERVAL"
        continue
    fi
    if gsa_armed; then
        echo "小愛與 GSA 模型皆已載入，雙喚醒就緒 $(date)" >> "$LOG"
        exit 0
    fi
    armed_rounds=$((armed_rounds + 1))
    try=$((try + 1))
    if [ "$armed_rounds" -ge "$GSA_GRACE" ]; then
        if [ "$fixes" -lt "$GSA_MAX_FIXES" ]; then
            pids=$(gsa_hotword_pids)
            if [ -n "$pids" ]; then
                fixes=$((fixes + 1))
                # 殺 isolated process（可能多個，空白分隔）；系統自動重綁
                "$KILL_BIN" $pids
                echo "try $try: GSA 模型未載入，重建 AoHD 鏈（kill $(echo $pids)，第 $fixes 次）$(date)" >> "$LOG"
                armed_rounds=0
            else
                echo "try $try: GSA 未載入且無 isolated hotword process（Voice Match 未啟用或尚未綁定），觀望 $(date)" >> "$LOG"
            fi
        fi
        sleep "$INTERVAL"
        continue
    fi
    echo "try $try: 小愛已武裝，等待 GSA 載入模型（第 $armed_rounds 輪）$(date)" >> "$LOG"
    sleep "$INTERVAL"
done
if xiaoai_armed; then
    echo "雙喚醒保底結束：GSA 模型仍未載入（重建 $fixes 次）$(date)" >> "$LOG"
else
    echo "雙喚醒保底放棄：小愛未武裝 $(date)" >> "$LOG"
fi
