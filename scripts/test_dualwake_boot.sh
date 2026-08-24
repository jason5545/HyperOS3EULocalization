#!/bin/sh
# 主機端測試 dualwake_boot.sh 的決策邏輯（不需裝置）。
# 用 PATH stub 取代 sleep/dumpsys/am/pgrep/kill，模擬 soundtrigger
# middleware 的 active model 清單與 GSA isolated hotword process 是否存在。
#
# 狀態檔（每輪由 sleep stub 遞增 round，round 從 0 開始；只有輪次間隔
# INTERVAL 的 sleep 才推進 round，同輪重試的短退避不推進）：
#   armed_after=N      第 N 輪起小愛模型出現在 active model 清單
#   gsa_armed_after=N  第 N 輪起 GSA 模型出現在 active model 清單
#   iso_after=N        第 N 輪起 isolated hotword process 存在（pgrep 有回應）
#   kill_heals=1       kill 發生後，GSA 模型於下一輪出現（模擬重建成功）
#   am_fails=N         前 N 次 am broadcast 失敗（模擬開機風暴 AMS 整批拒絕
#                      transaction：輸出與實機相同的 Failed transaction，rc=1）
#   vis_default=1      預設語音互動服務是 GSA（voiceinteraction dump 的
#                      processName 為 googlequicksearchbox:interactor）
#   vis_enrolled=1     voiceinteraction dump 帶 X Google 聲紋 vendorUuid
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKER="$ROOT/dualwake_boot.sh"
[ -f "$WORKER" ] || { echo "missing $WORKER" >&2; exit 1; }

PASS=0
FAIL=0

# run_worker ARMED_AFTER GSA_ARMED_AFTER ISO_AFTER KILL_HEALS GRACE MAX_TRIES AM_FAILS VIS_DEFAULT VIS_ENROLLED
run_worker() {
    TMP=$(mktemp -d)
    STUB="$TMP/stub"
    STATE="$TMP/state"
    mkdir -p "$STUB" "$STATE"
    echo 0 > "$STATE/round"
    echo "$1" > "$STATE/armed_after"
    echo "$2" > "$STATE/gsa_armed_after"
    echo "$3" > "$STATE/iso_after"
    echo "$4" > "$STATE/kill_heals"
    echo "$7" > "$STATE/am_fails"
    echo "$8" > "$STATE/vis_default"
    echo "$9" > "$STATE/vis_enrolled"

    cat > "$STUB/sleep" <<'EOF'
#!/bin/sh
# 只有輪次間隔的 sleep 才推進 round；同輪重試的短退避不推進
if [ "${1:-}" = "$STUB_ROUND_ARG" ]; then
    n=$(cat "$STUB_STATE/round")
    echo $((n + 1)) > "$STUB_STATE/round"
fi
EOF
    cat > "$STUB/dumpsys" <<'EOF'
#!/bin/sh
round=$(cat "$STUB_STATE/round")
case "$1" in
    soundtrigger_middleware)
        armed_after=$(cat "$STUB_STATE/armed_after")
        gsa_armed_after=$(cat "$STUB_STATE/gsa_armed_after")
        echo '##Service-Wide logs:'
        echo '##Active Session dumps:'
        if [ "$round" -ge "$armed_after" ]; then
            echo '0 ACTIVE PhraseSoundModel text: xiaoaitongxue'
        fi
        if [ "$round" -ge "$gsa_armed_after" ] || [ -f "$STUB_STATE/healed" ]; then
            echo '1 ACTIVE PhraseSoundModel text: X Google'
        fi
        echo '##Detached Session dumps:'
        ;;
    voiceinteraction)
        if [ "$(cat "$STUB_STATE/vis_default")" = "1" ]; then
            echo '  Service info:'
            echo '    processName=com.google.android.googlequicksearchbox:interactor'
        else
            echo '  Service info:'
            echo '    processName=com.miui.voiceassist'
        fi
        if [ "$(cat "$STUB_STATE/vis_enrolled")" = "1" ]; then
            echo '    vendor_uuid: 7038ddc8-30f2-11e6-b0ac-40a8f03d3f15'
        fi
        ;;
esac
EOF
    cat > "$STUB/pgrep" <<'EOF'
#!/bin/sh
round=$(cat "$STUB_STATE/round")
case "$*" in
    *interactor*)
        # GSA VIS 進程（gsa_vis_pid 的動態解析結果）
        [ "$(cat "$STUB_STATE/vis_default")" = "1" ] && echo 2222
        ;;
    *)
        iso_after=$(cat "$STUB_STATE/iso_after")
        if [ "$round" -ge "$iso_after" ]; then
            echo 1111
        fi
        ;;
esac
EOF
    cat > "$STUB/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >> "$STUB_STATE/calls"
if [ "$(cat "$STUB_STATE/kill_heals")" = "1" ]; then
    touch "$STUB_STATE/healed"
fi
EOF
    cat > "$STUB/am" <<'EOF'
#!/bin/sh
echo "am $*" >> "$STUB_STATE/calls"
n=0
[ -f "$STUB_STATE/am_count" ] && n=$(cat "$STUB_STATE/am_count")
n=$((n + 1))
echo "$n" > "$STUB_STATE/am_count"
if [ "$n" -le "$(cat "$STUB_STATE/am_fails")" ]; then
    # 與實機開機風暴時的輸出一致（cmd 失敗時印到 stderr、rc=1）
    echo "Broadcasting: Intent { act=android.intent.action.BOOT_COMPLETED }"
    echo "cmd: Failure calling service activity: Failed transaction (2147483646)" >&2
    exit 1
fi
echo "Broadcasting: Intent { act=android.intent.action.BOOT_COMPLETED }"
echo "Broadcast completed: result=0"
EOF
    chmod +x "$STUB"/sleep "$STUB"/dumpsys "$STUB"/pgrep "$STUB"/kill "$STUB"/am

    : > "$STATE/calls"
    STUB_STATE="$STATE" STUB_ROUND_ARG=0 PATH="$STUB:/usr/bin:/bin" \
        DUALWAKE_INTERVAL=0 DUALWAKE_RETRY_INTERVAL=7 \
        DUALWAKE_GSA_GRACE="$5" DUALWAKE_MAX_TRIES="$6" \
        DUALWAKE_KILL_BIN="$STUB/kill" DUALWAKE_LOG="$TMP/boot.log" \
        sh "$WORKER"
    cat "$STATE/calls"
    echo "--- log:"
    cat "$TMP/boot.log"
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

check_ge() { # check_ge DESC MIN ACTUAL（至少 MIN 次）
    if [ "$3" -ge "$2" ]; then
        PASS=$((PASS + 1))
        echo "ok: $1"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $1" >&2
        echo "  expected: >= $2" >&2
        echo "  actual:      $3" >&2
    fi
}

# --- 案例 1：小愛一直未武裝 -------------------------------------------------
OUT=$(run_worker 999 999 999 0 2 3 0 1 1)
check '未武裝:不重建 GSA' 0 "$(printf '%s' "$OUT" | count 'kill ')"
check '未武裝:每輪重送 BootupReceiver' 3 "$(printf '%s' "$OUT" | count 'am broadcast')"
check '未武裝:記錄放棄' 1 "$(printf '%s' "$OUT" | count '放棄')"

# --- 案例 2：雙方模型都在 → 完全不動 -----------------------------------------
OUT=$(run_worker 0 0 0 0 2 5 0 1 1)
check '雙方就緒:不重建' 0 "$(printf '%s' "$OUT" | count 'kill ')"
check '雙方就緒:不重送廣播' 0 "$(printf '%s' "$OUT" | count 'am broadcast')"
check '雙方就緒:記錄就緒' 1 "$(printf '%s' "$OUT" | count '就緒')"

# --- 案例 3:GSA 卡住 → 寬限後殺 isolated process,重建成功 --------------------
OUT=$(run_worker 0 999 0 1 2 8 0 1 1)
check 'GSA 卡住:重建一次' 1 "$(printf '%s' "$OUT" | count '^kill ')"
check 'GSA 卡住:殺的是 isolated hotword pid' 1 "$(printf '%s' "$OUT" | count '^kill 1111$')"
check 'GSA 卡住:重建後記錄就緒' 1 "$(printf '%s' "$OUT" | count '就緒')"
check 'GSA 卡住:不重送廣播' 0 "$(printf '%s' "$OUT" | count 'am broadcast')"

# --- 案例 4:重建也救不了 → 最多重建兩次後放棄 --------------------------------
OUT=$(run_worker 0 999 0 0 2 9 0 1 1)
check '重建無效:恰好重建兩次' 2 "$(printf '%s' "$OUT" | count '^kill ')"
check '重建無效:記錄仍未載入' 1 "$(printf '%s' "$OUT" | count 'GSA 模型仍未載入')"

# --- 案例 5:無 isolated process 且未註冊聲紋(Voice Match 關閉)→ 只觀望 ------
OUT=$(run_worker 0 999 999 0 2 6 0 1 0)
check '無 isolated process:不重建' 0 "$(printf '%s' "$OUT" | count 'kill ')"
check_ge '無 isolated process:記錄觀望' 1 "$(printf '%s' "$OUT" | count '觀望')"
check '無 isolated process:記錄結束' 1 "$(printf '%s' "$OUT" | count '仍未載入')"

# --- 案例 6:GSA 在寬限內自己載入 → 不重建 ------------------------------------
OUT=$(run_worker 0 3 0 0 4 6 0 1 1)
check 'GSA 慢來:不重建' 0 "$(printf '%s' "$OUT" | count 'kill ')"
check 'GSA 慢來:記錄就緒' 1 "$(printf '%s' "$OUT" | count '就緒')"
check_ge 'GSA 慢來:有等待紀錄' 1 "$(printf '%s' "$OUT" | count '等待 GSA 載入')"

# --- 案例 7:小愛第 2 輪才武裝,GSA 早已就緒 ------------------------------------
OUT=$(run_worker 2 0 0 0 2 6 0 1 1)
check '小愛晚武裝:先重送兩次廣播' 2 "$(printf '%s' "$OUT" | count 'am broadcast')"
check '小愛晚武裝:不重建' 0 "$(printf '%s' "$OUT" | count 'kill ')"
check '小愛晚武裝:記錄就緒' 1 "$(printf '%s' "$OUT" | count '就緒')"

# --- 案例 8:worker 程式碼不可再呼叫 killall/audioserver(註解除外) -------------
check 'worker 不含 audioserver bounce' 0 "$(grep -vE '^[[:space:]]*#' "$WORKER" | grep -cE 'killall|audioserver' || true)"

# --- 案例 9:am 前兩次失敗(開機風暴 Failed transaction)→ 同輪重試成功 ---------
# 2026-08-22 實機：連續三輪重送全部 Failed transaction (2147483646)，
# 一輪一次的舊實作在風暴期間完全幫不上忙。
OUT=$(run_worker 1 0 0 0 2 6 2 1 1)
check 'am 失敗:同輪重試到成功共 3 次' 3 "$(printf '%s' "$OUT" | count 'am broadcast')"
check 'am 失敗:記錄兩次失敗' 2 "$(printf '%s' "$OUT" | count '次失敗')"
check 'am 失敗:記錄一次送出成功' 1 "$(printf '%s' "$OUT" | count '送出成功')"
check 'am 失敗:失敗訊息寫進記錄' 2 "$(printf '%s' "$OUT" | count 'Failed transaction')"
check 'am 失敗:記錄就緒' 1 "$(printf '%s' "$OUT" | count '就緒')"
check 'am 失敗:不重建' 0 "$(printf '%s' "$OUT" | count 'kill ')"

# --- 案例 10:am 持續失敗 → 每輪試滿 RETRY_MAX 次,不提早崩潰 -------------------
OUT=$(run_worker 999 999 999 0 2 3 999 1 1)
check 'am 全敗:三輪共 9 次嘗試' 9 "$(printf '%s' "$OUT" | count 'am broadcast')"
check 'am 全敗:記錄九次失敗' 9 "$(printf '%s' "$OUT" | count '次失敗')"
check 'am 全敗:記錄放棄' 1 "$(printf '%s' "$OUT" | count '放棄')"
check 'am 全敗:不重建 GSA' 0 "$(printf '%s' "$OUT" | count 'kill ')"

# --- 案例 11:無 isolated process 的 AoHD wedge → 殺 VIS 進程,重建成功 --------
# 2026-08-24 實機：ATTACH 後 HDS 連線從未建立（No Hotword detection
# connection），卡 4.5 小時；殺 :interactor 後 6 秒 re-ATTACH、模型載入。
OUT=$(run_worker 0 999 999 1 2 8 0 1 1)
check 'VIS wedge:重建一次' 1 "$(printf '%s' "$OUT" | count '^kill ')"
check 'VIS wedge:殺的是 VIS pid' 1 "$(printf '%s' "$OUT" | count '^kill 2222$')"
check 'VIS wedge:不去殺不存在的 isolated process' 0 "$(printf '%s' "$OUT" | count '^kill 1111$')"
check 'VIS wedge:記錄 VIS 重建' 1 "$(printf '%s' "$OUT" | count '殺 VIS 進程重建')"
check 'VIS wedge:重建後記錄就緒' 1 "$(printf '%s' "$OUT" | count '就緒')"

# --- 案例 12:VIS kill 也救不了 → 與 isolated kill 共用額度,兩次後放棄 ---------
OUT=$(run_worker 0 999 999 0 2 9 0 1 1)
check 'VIS 重建無效:恰好重建兩次' 2 "$(printf '%s' "$OUT" | count '^kill 2222$')"
check 'VIS 重建無效:總重建恰兩次' 2 "$(printf '%s' "$OUT" | count '^kill ')"
check 'VIS 重建無效:記錄仍未載入' 1 "$(printf '%s' "$OUT" | count 'GSA 模型仍未載入')"

# --- 案例 13:無 isolated process 但預設助理不是 GSA → 只觀望不動手 ------------
OUT=$(run_worker 0 999 999 0 2 6 0 0 1)
check 'GSA 非預設 VIS:不重建' 0 "$(printf '%s' "$OUT" | count 'kill ')"
check_ge 'GSA 非預設 VIS:記錄觀望' 1 "$(printf '%s' "$OUT" | count '觀望')"
check 'GSA 非預設 VIS:記錄結束' 1 "$(printf '%s' "$OUT" | count '仍未載入')"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
