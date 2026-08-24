#!/bin/sh
# 主機端測試 mount_scrub.sh 的決策邏輯（不需裝置）。
# 用 PATH stub 取代 pgrep/nsenter/sleep；procfs 用假目錄代替，mountinfo
# 內容的形狀取自 2026-08-24 myron 實機（functionfs adbd 誘餌、mi_ext
# overlay、/adb/modules 手動 bind 三類都覆蓋）。
#
# 狀態檔：
#   heal=1   nsenter stub 假裝 umount 成功，把該掛載點從假 mountinfo 移除
#            （用來驗證第二輪不重複 scrub）
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKER="$ROOT/mount_scrub.sh"
[ -f "$WORKER" ] || { echo "missing $WORKER" >&2; exit 1; }

PASS=0
FAIL=0

# mkproc PID CMDLINE — 在 $PROC 下建假進程；mountinfo 內容由 stdin 給
mkproc() {
    mkdir -p "$PROC/$1"
    printf '%s' "$2" > "$PROC/$1/cmdline"
    cat > "$PROC/$1/mountinfo"
    echo "$1" >> "$STATE/pids"
}

# run_worker ROUNDS HEAL SETUP_FN
run_worker() {
    TMP=$(mktemp -d)
    STUB="$TMP/stub"
    STATE="$TMP/state"
    PROC="$TMP/proc"
    mkdir -p "$STUB" "$STATE" "$PROC"
    : > "$STATE/pids"
    echo "$2" > "$STATE/heal"

    $3

    cat > "$STUB/pgrep" <<'EOF'
#!/bin/sh
cat "$STUB_STATE/pids"
EOF
    cat > "$STUB/nsenter" <<'EOF'
#!/bin/sh
echo "nsenter $*" >> "$STUB_STATE/calls"
# 解析 -t PID 與最後一個參數（掛載點）；heal=1 時把它從假 mountinfo 移除
pid=""
prev=""
for a in "$@"; do
    [ "$prev" = "-t" ] && pid="$a"
    prev="$a"
done
mp=""
for a in "$@"; do mp="$a"; done
if [ "$(cat "$STUB_STATE/heal")" = "1" ] && [ -n "$pid" ] && \
   [ -f "$STUB_PROC/$pid/mountinfo" ]; then
    awk -v mp="$mp" '$5 != mp' "$STUB_PROC/$pid/mountinfo" \
        > "$STUB_PROC/$pid/mountinfo.tmp"
    mv "$STUB_PROC/$pid/mountinfo.tmp" "$STUB_PROC/$pid/mountinfo"
fi
EOF
    cat > "$STUB/sleep" <<'EOF'
#!/bin/sh
:
EOF
    chmod +x "$STUB"/pgrep "$STUB"/nsenter "$STUB"/sleep

    : > "$STATE/calls"
    STUB_STATE="$STATE" STUB_PROC="$PROC" PATH="$STUB:/usr/bin:/bin" \
        MOUNT_SCRUB_INTERVAL=0 MOUNT_SCRUB_MAX_ROUNDS="$1" \
        MOUNT_SCRUB_PROC_ROOT="$PROC" MOUNT_SCRUB_LOG="$TMP/scrub.log" \
        sh "$WORKER"
    echo "rc=$?"
    cat "$STATE/calls"
    echo "--- log:"
    cat "$TMP/scrub.log" 2>/dev/null || true
    rm -rf "$TMP"
}

check() { # check DESC EXPECTED ACTUAL
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
        echo "ok: $1"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $1" >&2
        echo "  expected: $2" >&2
        echo "  actual:   $3" >&2
    fi
}

count() { grep -c "$1" | tr -d ' '; }

# --- 案例 1:namespace 乾淨 → 完全不動 ----------------------------------------
setup_clean() {
    mkproc 1001 com.google.android.gms <<'EOF'
13402 13394 0:152 / /dev/usb-ffs/adb rw,relatime master:71 - functionfs adb rw
13439 13438 0:107 / /system_ext/etc/permissions ro,relatime master:54 - overlay overlay ro,lowerdir=/mnt/vendor/mi_ext
EOF
}
OUT=$(run_worker 1 0 setup_clean)
check '乾淨:不 scrub' 0 "$(printf '%s' "$OUT" | count 'nsenter ')"
check '乾淨:正常結束' 1 "$(printf '%s' "$OUT" | count 'rc=0')"

# --- 案例 2:單一進程兩條洩露 + 誘餌 → 只清該兩條 -----------------------------
setup_two_binds() {
    mkproc 1001 com.google.android.gms <<'EOF'
13402 13394 0:152 / /dev/usb-ffs/adb rw,relatime master:71 - functionfs adb rw
13439 13438 0:107 / /system_ext/etc/permissions ro,relatime master:54 - overlay overlay ro,lowerdir=/mnt/vendor/mi_ext
36719 13470 254:46 /adb/modules/BW_Audio_K90PM/dolby/curves/fusion.xml /odm/etc/dolby/dax-default.xml rw,nosuid,nodev,noatime master:69 - f2fs /dev/block/dm-46 rw
36720 13470 254:46 /adb/modules/com.google.android.youtube-morphe/com.google.android.youtube.apk /data/app/~~abc==/com.google.android.youtube-x==/base.apk rw master:69 - f2fs /dev/block/dm-46 rw
EOF
}
OUT=$(run_worker 1 0 setup_two_binds)
check '兩條洩露:恰好兩次 umount' 2 "$(printf '%s' "$OUT" | count ' umount ')"
check '兩條洩露:打對 pid' 2 "$(printf '%s' "$OUT" | count 'nsenter -t 1001 ')"
check '兩條洩露:清 dolby 掛載點' 1 "$(printf '%s' "$OUT" | count 'umount -l /odm/etc/dolby/dax-default.xml')"
check '兩條洩露:清 morphe 掛載點' 1 "$(printf '%s' "$OUT" | count 'umount -l /data/app/~~abc==/com.google.android.youtube-x==/base.apk')"
check '兩條洩露:不碰 functionfs adbd' 0 "$(printf '%s' "$OUT" | count 'usb-ffs')"
check '兩條洩露:不碰 overlay' 0 "$(printf '%s' "$OUT" | count 'system_ext')"
check '兩條洩露:記錄含 pkg 與掛載點' 1 "$(printf '%s' "$OUT" | count 'scrubbed com.google.android.gms(1001): /odm/etc/dolby/dax-default.xml')"

# --- 案例 3:多進程（gms / gms.unstable / wallet）→ 全清 -----------------------
setup_multi() {
    mkproc 1001 com.google.android.gms <<'EOF'
36719 13470 254:46 /adb/modules/BW_Audio_K90PM/dolby/curves/fusion.xml /odm/etc/dolby/dax-default.xml rw master:69 - f2fs /dev/block/dm-46 rw
EOF
    mkproc 1002 com.google.android.gms.unstable <<'EOF'
36719 13470 254:46 /adb/modules/BW_Audio_K90PM/dolby/curves/fusion.xml /odm/etc/dolby/dax-default.xml rw master:69 - f2fs /dev/block/dm-46 rw
EOF
    mkproc 1003 com.google.android.apps.walletnfcrel <<'EOF'
36720 13470 254:46 /adb/modules/org.zwanoo.android.speedtest-morphe/a.apk /data/app/x/base.apk rw master:69 - f2fs /dev/block/dm-46 rw
EOF
}
OUT=$(run_worker 1 0 setup_multi)
check '多進程:三次 umount' 3 "$(printf '%s' "$OUT" | count ' umount ')"
check '多進程:gms.unstable 也清' 1 "$(printf '%s' "$OUT" | count 'nsenter -t 1002 ')"
check '多進程:wallet 也清' 1 "$(printf '%s' "$OUT" | count 'nsenter -t 1003 ')"

# --- 案例 4:進程中途死掉（無 mountinfo）→ 跳過不崩潰 --------------------------
setup_dead() {
    echo 1999 >> "$STATE/pids"
}
OUT=$(run_worker 1 0 setup_dead)
check '死進程:不 scrub' 0 "$(printf '%s' "$OUT" | count 'nsenter ')"
check '死進程:正常結束' 1 "$(printf '%s' "$OUT" | count 'rc=0')"

# --- 案例 5:兩輪,heal 後第二輪不重複 ------------------------------------------
OUT=$(run_worker 2 1 setup_two_binds)
check '兩輪:不重複 scrub' 2 "$(printf '%s' "$OUT" | count ' umount ')"

# --- 案例 6:同一掛載點重複出現 → 只 umount 一次 -------------------------------
setup_dup() {
    mkproc 1001 com.google.android.gms <<'EOF'
36719 13470 254:46 /adb/modules/BW_Audio_K90PM/dolby/curves/fusion.xml /odm/etc/dolby/dax-default.xml rw master:69 - f2fs /dev/block/dm-46 rw
36721 13470 254:46 /adb/modules/BW_Audio_K90PM/dolby/curves/fusion.xml /odm/etc/dolby/dax-default.xml rw master:69 - f2fs /dev/block/dm-46 rw
EOF
}
OUT=$(run_worker 1 0 setup_dup)
check '重複掛載點:只清一次' 1 "$(printf '%s' "$OUT" | count ' umount ')"

# --- 案例 7:靜態檢查 — pgrep pattern 必須錨定（防誤清 root namespace）---------
BAD=$(grep -vE '^[[:space:]]*#' "$WORKER" | grep 'pgrep' | grep -vc 'pgrep -f "$TARGETS"')
check 'worker pgrep 只用錨定 TARGETS' 0 "$BAD"
ANCHORS=$(grep '^TARGETS=' "$WORKER" | grep -o '\^com' | wc -l | tr -d ' ')
check 'TARGETS 三族皆錨定開頭' 3 "$ANCHORS"
check 'TARGETS 不含金融進程' 0 "$(grep '^TARGETS=' "$WORKER" | grep -cE 'cathay|bank|jkos' || true)"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
