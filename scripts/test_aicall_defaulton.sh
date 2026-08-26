#!/bin/sh
# 主機端測試 aicall_defaulton.sh 的決策邏輯（不需裝置）。
# PATH stub 取代 appops/chown/chmod/pidof；prefs 目錄用假目錄代替，
# setting.xml 的形狀取自 2026-08-26 myron 實機（data_migrated 等 key）。
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

write_prefs_nokey() {
    mkdata
    cat > "$TMP/user/0/$PKG/shared_prefs/setting.xml" <<'EOF'
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <boolean name="data_migrated" value="true" />
    <boolean name="set_notification_show_keyguard" value="true" />
    <int name="data_transfer_version" value="1" />
</map>
EOF
}

# --- 案例 1:既有 setting.xml 無 key → 插入 true、其他 key 保留 ----------------
OUT=$(run_worker default write_prefs_nokey)
PREFS=$(printf '%s' "$OUT" | sed -n '/^--- prefs:/,/^--- log:/p')
check '無key:插入 incallctrlbutton' 1 "$(printf '%s' "$PREFS" | count 'name="incallctrlbutton" value="true"')"
check '無key:原有 key 保留' 1 "$(printf '%s' "$PREFS" | count 'name="data_migrated"')"
check '無key:插入在 </map> 前' before "$(printf '%s' "$PREFS" | awk '/incallctrlbutton/{f=1} /<\/map>/{print (f?"before":"after"); exit}')"
check '無key:chown 比照目錄' 1 "$(printf '%s' "$OUT" | count '^chown ')"
check '無key:chmod 0660' 1 "$(printf '%s' "$OUT" | count 'chmod 0660')"
check '無key:log 記 OK' 1 "$(printf '%s' "$OUT" | count 'OK: incallctrlbutton=true')"

# --- 案例 2:key=true 已存在 → 完全不動 ----------------------------------------
setup_on() {
    write_prefs_nokey
    printf '    <boolean name="incallctrlbutton" value="true" />\n' >> "$TMP/user/0/$PKG/shared_prefs/setting.xml"
}
OUT=$(run_worker default setup_on)
check '已開:不寫檔' 0 "$(printf '%s' "$OUT" | count '^chown ')"
check '已開:不碰 appops' 0 "$(printf '%s' "$OUT" | count 'appops set')"
check '已開:不寫 log' 0 "$(printf '%s' "$OUT" | count 'OK:')"

# --- 案例 3:key=false 已存在（使用者明確關）→ 尊重不動 -------------------------
setup_off() {
    write_prefs_nokey
    printf '    <boolean name="incallctrlbutton" value="false" />\n' >> "$TMP/user/0/$PKG/shared_prefs/setting.xml"
}
OUT=$(run_worker default setup_off)
PREFS=$(printf '%s' "$OUT" | sed -n '/^--- prefs:/,/^--- log:/p')
check '已關:不寫檔' 0 "$(printf '%s' "$OUT" | count '^chown ')"
check '已關:不改成 true' 0 "$(printf '%s' "$PREFS" | count 'name="incallctrlbutton" value="true"' || true)"
check '已關:false 仍在' 1 "$(printf '%s' "$PREFS" | count 'name="incallctrlbutton" value="false"')"

# --- 案例 4:setting.xml 不存在但 data dir 在 → 建新檔、補 owner/mode -----------
OUT=$(run_worker default mkdata)
PREFS=$(printf '%s' "$OUT" | sed -n '/^--- prefs:/,/^--- log:/p')
check '新建:有 XML header' 1 "$(printf '%s' "$PREFS" | count '<?xml version')"
check '新建:有 key' 1 "$(printf '%s' "$PREFS" | count 'name="incallctrlbutton" value="true"')"
check '新建:有 </map>' 1 "$(printf '%s' "$PREFS" | count '</map>')"
check '新建:chown 比照目錄' 1 "$(printf '%s' "$OUT" | count '^chown ')"
check '新建:chmod 0660' 1 "$(printf '%s' "$OUT" | count 'chmod 0660')"

# --- 案例 5:data dir 不存在 → SKIP、完全不動 -----------------------------------
OUT=$(run_worker default :)
check '無dir:不寫檔' 0 "$(printf '%s' "$OUT" | count '^chown ')"
check '無dir:不建檔' 1 "$(printf '%s' "$OUT" | count '(no file)')"
check '無dir:log 記 SKIP' 1 "$(printf '%s' "$OUT" | count 'SKIP:')"

# --- 案例 6:appops 已 allow → 不重複 set ---------------------------------------
OUT=$(run_worker allow write_prefs_nokey)
check 'ops allow:不 set' 0 "$(printf '%s' "$OUT" | count 'appops set')"
check 'ops allow:pref 照補' 1 "$(printf '%s' "$OUT" | count 'name="incallctrlbutton" value="true"')"

# --- 案例 7:appops deny（使用者明確拒絕）→ 不覆寫 -------------------------------
OUT=$(run_worker deny write_prefs_nokey)
check 'ops deny:不 set' 0 "$(printf '%s' "$OUT" | count 'appops set')"

# --- 案例 8:appops default → set allow ------------------------------------------
OUT=$(run_worker default write_prefs_nokey)
check 'ops default:set allow' 1 "$(printf '%s' "$OUT" | count 'appops set com.xiaomi.aiasst.service SYSTEM_ALERT_WINDOW allow')"

# --- 案例 9:App 在跑（pidof 有 pid）→ kill 重讀、log 有記錄 ---------------------
setup_running() {
    write_prefs_nokey
    echo 12345 > "$STATE/pid"
}
OUT=$(run_worker default setup_running)
check '在跑:log 記 KILL' 1 "$(printf '%s' "$OUT" | count 'KILL: com.xiaomi.aiasst.service(12345)')"
check '在跑:pref 照補' 1 "$(printf '%s' "$OUT" | count 'name="incallctrlbutton" value="true"')"

# --- 案例 10:App 沒跑 → 不記 KILL ----------------------------------------------
OUT=$(run_worker default write_prefs_nokey)
check '沒跑:不記 KILL' 0 "$(printf '%s' "$OUT" | count 'KILL:')"

# --- 案例 11:靜態檢查 — 絕對禁止的兩個寫法 --------------------------------------
# am force-stop 會把套件標 stopped → resolveActivity 失效 → 入口消失
check '不用 am force-stop' 0 "$(grep -v '^#' "$WORKER" | grep -c 'am force-stop' || true)"
# toybox sed -i 建新 inode → owner 變 root → App 讀不到 prefs
check '不用 sed -i' 0 "$(grep -v '^#' "$WORKER" | grep -c 'sed -i' || true)"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
