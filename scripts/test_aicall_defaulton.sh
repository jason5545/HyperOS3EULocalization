#!/bin/sh
# 主機端測試 aicall_defaulton.sh 的決策邏輯（不需裝置）。
# PATH stub 取代 appops/chown/chmod/pidof；prefs 目錄用假目錄代替，
# setting.xml 的形狀取自 2026-08-26 myron 實機（data_migrated 等 key）。
# 政策（v1.0.33 起）：四個 key 無條件全 true——明確 false 也翻回；
# 全 true 時才跳過寫檔（冪等，不殺進程）。appops 無條件補 allow。
#
# 狀態檔：
#   pid=<pid>   pidof stub 回傳該 pid（模擬 App 在跑；預設空 = 未跑）
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORKER="$ROOT/aicall_defaulton.sh"
[ -f "$WORKER" ] || { echo "missing $WORKER" >&2; exit 1; }

PASS=0
FAIL=0
PKG=com.xiaomi.aiasst.service
KEYS="aicall_onoff callscreen_onoff incallctrlbutton privacy"

# run_worker APPOPS_MODE SETUP_FN
run_worker() {
    TMP=$(mktemp -d)
    STUB="$TMP/stub"
    STATE="$TMP/state"
    mkdir -p "$STUB" "$STATE"
    echo "$1" > "$STATE/appops"
    : > "$STATE/pid"

    $2

    cat > "$STUB/appops" <<'EOF'
#!/bin/sh
# appops get PKG OP → "SYSTEM_ALERT_WINDOW: <mode>"；set 記進 calls
if [ "$1" = get ]; then
    echo "SYSTEM_ALERT_WINDOW: $(cat "$STUB_STATE/appops")"
else
    echo "appops $*" >> "$STUB_STATE/calls"
fi
EOF
    cat > "$STUB/pidof" <<'EOF'
#!/bin/sh
cat "$STUB_STATE/pid"
EOF
    for c in chown chmod; do
        cat > "$STUB/$c" <<EOF
#!/bin/sh
echo "$c \$*" >> "\$STUB_STATE/calls"
EOF
    done
    chmod +x "$STUB"/appops "$STUB"/pidof "$STUB"/chown "$STUB"/chmod

    : > "$STATE/calls"
    STUB_STATE="$STATE" PATH="$STUB:/usr/bin:/bin" \
        AICALL_USER_DIR="$TMP/user/0" AICALL_DEFAULTON_LOG="$TMP/aicall.log" \
        sh "$WORKER"
    echo "rc=$?"
    echo "--- calls:"
    cat "$STATE/calls"
    echo "--- prefs:"
    cat "$TMP/user/0/$PKG/shared_prefs/setting.xml" 2>/dev/null || echo "(no file)"
    echo "--- log:"
    cat "$TMP/aicall.log" 2>/dev/null || true
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

mkdata() { mkdir -p "$TMP/user/0/$PKG/shared_prefs"; }

# write_prefs_with [name=value ...] — 產生形狀正確的 setting.xml（key 在 <map> 內）
write_prefs_with() {
    mkdata
    {
        printf '%s\n' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>"
        printf '%s\n' "<map>"
        printf '%s\n' '    <boolean name="data_migrated" value="true" />'
        printf '%s\n' '    <boolean name="set_notification_show_keyguard" value="true" />'
        printf '%s\n' '    <int name="data_transfer_version" value="1" />'
        for kv in "$@"; do
            printf '    <boolean name="%s" value="%s" />\n' "${kv%%=*}" "${kv#*=}"
        done
        printf '%s\n' "</map>"
    } > "$TMP/user/0/$PKG/shared_prefs/setting.xml"
}

write_prefs_nokey() { write_prefs_with; }

# --- 案例 1:既有 setting.xml 無 key → 四 key 全插入 true、其他 key 保留 --------
OUT=$(run_worker default write_prefs_nokey)
PREFS=$(printf '%s' "$OUT" | sed -n '/^--- prefs:/,/^--- log:/p')
for k in $KEYS; do
    check "無key:插入 $k" 1 "$(printf '%s' "$PREFS" | count "name=\"$k\" value=\"true\"")"
done
check '無key:原有 key 保留' 1 "$(printf '%s' "$PREFS" | count 'name="data_migrated"')"
check '無key:插入在 </map> 前' before "$(printf '%s' "$PREFS" | awk '/aicall_onoff/{f=1} /<\/map>/{print (f?"before":"after"); exit}')"
check '無key:chown 比照目錄' 1 "$(printf '%s' "$OUT" | count '^chown ')"
check '無key:chmod 0660' 1 "$(printf '%s' "$OUT" | count 'chmod 0660')"
check '無key:log 記 OK' 1 "$(printf '%s' "$OUT" | count 'OK: AI 通話四鍵全開')"

# --- 案例 2:key=false 已存在 → 直接翻回 true（無條件開啟，不尊重明確關）-------
setup_off() {
    write_prefs_with aicall_onoff=false privacy=false
}
OUT=$(run_worker default setup_off)
PREFS=$(printf '%s' "$OUT" | sed -n '/^--- prefs:/,/^--- log:/p')
check '已關:aicall_onoff 翻 true' 1 "$(printf '%s' "$PREFS" | count 'name="aicall_onoff" value="true"')"
check '已關:false 行被吃掉' 0 "$(printf '%s' "$PREFS" | count 'value="false"' || true)"
check '已關:privacy 翻 true' 1 "$(printf '%s' "$PREFS" | count 'name="privacy" value="true"')"
check '已關:aicall_onoff 無重複行' 1 "$(printf '%s' "$PREFS" | count 'name="aicall_onoff"')"

# --- 案例 3:四 key 全 true → 不寫檔、不殺進程（冪等）；appops 仍按需補 ----------
setup_allon() {
    write_prefs_with aicall_onoff=true callscreen_onoff=true incallctrlbutton=true privacy=true
    echo 12345 > "$STATE/pid"
}
OUT=$(run_worker allow setup_allon)
check '全開:不寫檔' 0 "$(printf '%s' "$OUT" | count '^chown ')"
check '全開+allow:不碰 appops' 0 "$(printf '%s' "$OUT" | count 'appops set')"
check '全開:不殺進程' 0 "$(printf '%s' "$OUT" | count 'KILL:')"
check '全開:不寫 OK log' 0 "$(printf '%s' "$OUT" | count 'OK:')"
OUT=$(run_worker default setup_allon)
check '全開+default:appops 照補' 1 "$(printf '%s' "$OUT" | count 'appops set com.xiaomi.aiasst.service SYSTEM_ALERT_WINDOW allow')"
check '全開+default:仍不寫檔' 0 "$(printf '%s' "$OUT" | count '^chown ')"

# --- 案例 4:setting.xml 不存在但 data dir 在 → 建新檔、補 owner/mode -----------
OUT=$(run_worker default mkdata)
PREFS=$(printf '%s' "$OUT" | sed -n '/^--- prefs:/,/^--- log:/p')
check '新建:有 XML header' 1 "$(printf '%s' "$PREFS" | count '<?xml version')"
for k in $KEYS; do
    check "新建:有 $k" 1 "$(printf '%s' "$PREFS" | count "name=\"$k\" value=\"true\"")"
done
check '新建:有 </map>' 1 "$(printf '%s' "$PREFS" | count '</map>')"
check '新建:chown 比照目錄' 1 "$(printf '%s' "$OUT" | count '^chown ')"
check '新建:chmod 0660' 1 "$(printf '%s' "$OUT" | count 'chmod 0660')"

# --- 案例 5:data dir 不存在 → SKIP、完全不動 -----------------------------------
OUT=$(run_worker default :)
check '無dir:不寫檔' 0 "$(printf '%s' "$OUT" | count '^chown ')"
check '無dir:不建檔' 1 "$(printf '%s' "$OUT" | count '(no file)')"
check '無dir:log 記 SKIP' 1 "$(printf '%s' "$OUT" | count 'SKIP:')"

# --- 案例 6:appops deny → 也 set allow（無條件，不尊重明確拒絕）------------------
OUT=$(run_worker deny write_prefs_nokey)
check 'ops deny:照樣 set allow' 1 "$(printf '%s' "$OUT" | count 'appops set com.xiaomi.aiasst.service SYSTEM_ALERT_WINDOW allow')"

# --- 案例 7:appops default → set allow ------------------------------------------
OUT=$(run_worker default write_prefs_nokey)
check 'ops default:set allow' 1 "$(printf '%s' "$OUT" | count 'appops set com.xiaomi.aiasst.service SYSTEM_ALERT_WINDOW allow')"

# --- 案例 8:appops 已 allow → 不重複 set -----------------------------------------
OUT=$(run_worker allow write_prefs_nokey)
check 'ops allow:不 set' 0 "$(printf '%s' "$OUT" | count 'appops set')"
check 'ops allow:pref 照補' 1 "$(printf '%s' "$OUT" | count 'name="aicall_onoff" value="true"')"

# --- 案例 9:App 在跑（pidof 有 pid）→ kill 重讀、log 有記錄 ----------------------
setup_running() {
    write_prefs_nokey
    echo 12345 > "$STATE/pid"
}
OUT=$(run_worker default setup_running)
check '在跑:log 記 KILL' 1 "$(printf '%s' "$OUT" | count 'KILL: com.xiaomi.aiasst.service(12345)')"
check '在跑:pref 照補' 1 "$(printf '%s' "$OUT" | count 'name="aicall_onoff" value="true"')"

# --- 案例 10:App 沒跑 → 不記 KILL -----------------------------------------------
OUT=$(run_worker default write_prefs_nokey)
check '沒跑:不記 KILL' 0 "$(printf '%s' "$OUT" | count 'KILL:')"

# --- 案例 11:靜態檢查 — 絕對禁止的兩個寫法 ---------------------------------------
# am force-stop 會把套件標 stopped → resolveActivity 失效 → 入口消失
check '不用 am force-stop' 0 "$(grep -v '^#' "$WORKER" | grep -c 'am force-stop' || true)"
# toybox sed -i 建新 inode → owner 變 root → App 讀不到 prefs
check '不用 sed -i' 0 "$(grep -v '^#' "$WORKER" | grep -c 'sed -i' || true)"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
