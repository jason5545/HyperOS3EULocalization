#!/system/bin/sh
# 敏感進程 mount namespace 清道夫（由 service.sh 複製到 /data/local/tmp 後
# 以 nohup 背景啟動；service.sh 本體立刻結束，不常駐帶模組路徑的 root shell）。
#
# 背景（2026-08-24 myron 實測定因）：KSU 的 per-app umount 只在 app
# specialize（進程誕生）那一刻執行一次，且只涵蓋 KSU 自己追蹤的模組掛載。
# 其他模組腳本手動下的 mount --bind（BW_Audio 的 dolby/quasar XML——且其
# watchdog 每次 audioserver 重啟都會重新 bind；morphe 修補版 APK bind 到
# base.apk）不是 KSU 追蹤的掛載，specialize 時清不掉；更糟的是 runtime
# 重新 bind 會經 shared mount propagation（peer group）滲入「還活著」的
# 長壽進程（gms.persistent / gms 幾乎不死的 mount namespace，
# /proc/self/mountinfo 出現來源 /adb/modules/... 的條目，DroidGuard 一讀
# 即判 root。這正是「Wallet/GMS 有時跳 root 提示、殺掉重開就好」的成因：
# 重開 = 重新 specialize = umount 清掉，之後又被 propagation 弄髒。
#
# 對策：週期性把這三族進程（與 zygisk-src/main.cpp isSensitiveProcess
# 相同名單）namespace 裡來源在 /data/adb/modules 下的掛載 umount -l 掉
# ——效果等同 KSU umount，但涵蓋 specialize 之後的滲入。只清 app 自己
# namespace 裡的傳播副本，root namespace 的掛載本體與其他 app 完全不受
# 影響（實測：scrub 後 pid 1 的 69 條掛載原封不動，音訊功能正常）。
#
# 安全不變量：
# - 目標 pattern 必須錨定開頭（^）： adb/su shell 的 cmdline 若含套件名，
#   未錨定的 pgrep -f 會比到 shell 本身，nsenter 進去 umount 等於拆 root
#   namespace 的掛載本體（2026-08-24 手動驗證時親眼確認風險）。
# - 只比對 mountinfo 第 4 欄（來源檔案系統內路徑）為 /adb/modules/ 開頭
#   的條目；adbd 的 functionfs（字尾含 adb）、KSU/hybrid 的 overlay 都不是
#   這個形狀，絕不誤傷。
# - 金融進程（銀行 RASP 會抓「被 umount」狀態本身）不在名單內，絕不可加。
#
# 環境變數（主要給主機端測試用）：
#   MOUNT_SCRUB_INTERVAL    每輪間隔秒數（預設 15）
#   MOUNT_SCRUB_MAX_ROUNDS  最多幾輪（預設 0 = 無限；測試用）
#   MOUNT_SCRUB_PROC_ROOT   procfs 根目錄（預設 /proc）
#   MOUNT_SCRUB_NSENTER     nsenter 指令路徑（預設 nsenter）
#   MOUNT_SCRUB_LOG         記錄檔路徑（預設 /dev/null）

INTERVAL="${MOUNT_SCRUB_INTERVAL:-15}"
MAX_ROUNDS="${MOUNT_SCRUB_MAX_ROUNDS:-0}"
PROC_ROOT="${MOUNT_SCRUB_PROC_ROOT:-/proc}"
NSENTER="${MOUNT_SCRUB_NSENTER:-nsenter}"
LOG="${MOUNT_SCRUB_LOG:-/dev/null}"

# 與 main.cpp isSensitiveProcess 同步：gms（含 :/. 子進程）、wallet、vending。
TARGETS='^com\.google\.android\.gms|^com\.google\.android\.apps\.walletnfcrel|^com\.android\.vending'

log_line() {
    echo "[$(date '+%m-%d %H:%M:%S')] $*" >> "$LOG"
}

scrub_once() {
    for pid in $(pgrep -f "$TARGETS" 2>/dev/null); do
        MOUNTINFO="$PROC_ROOT/$pid/mountinfo"
        [ -r "$MOUNTINFO" ] || continue
        # 第 4 欄 = 來源檔案系統內的路徑：/data 分割區上的 /data/adb/modules
        # 在此顯示為 /adb/modules。取其掛載點（第 5 欄）lazy umount。
        HITS=$(awk '$4 ~ "^/adb/modules/" { print $5 }' "$MOUNTINFO" 2>/dev/null | sort -u)
        [ -n "$HITS" ] || continue
        PKG=$(tr -d '\0' < "$PROC_ROOT/$pid/cmdline" 2>/dev/null)
        echo "$HITS" | while read -r MP; do
            [ -n "$MP" ] || continue
            # toybox nsenter 會把 umount 的 -l 誤認成自己的選項，必須用 -- 隔開
            if "$NSENTER" -t "$pid" -m -- umount -l "$MP" 2>/dev/null; then
                log_line "scrubbed ${PKG}(${pid}): ${MP}"
            fi
        done
    done
}

ROUND=0
while :; do
    scrub_once
    ROUND=$((ROUND + 1))
    if [ "$MAX_ROUNDS" -gt 0 ] && [ "$ROUND" -ge "$MAX_ROUNDS" ]; then
        exit 0
    fi
    sleep "$INTERVAL"
done
