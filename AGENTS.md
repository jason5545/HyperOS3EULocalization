# AGENTS.md

Magisk/KernelSU module: HyperOS 3 EU localization — XiaoAI voice stack, Taplus
(傳送門), Mi Pay chain, ThemeManager region, dual hotword wake.

## Layout

- `system/product/…` — systemless payload APKs (checked by `build.sh`)
- `system/product/etc/permissions/` — priv-app grant XMLs (defensive; the
  xiaomi.eu 308 base already ships these grants)
- `payload/` — data-app APKs installed by the on-device service script
- `zygisk-src/` — Zygisk module (`main.cpp` = Taplus INTL flip + sensitive-process
  policy, `dualwake.cpp` = dual wake, `homefeed.cpp` = MiuiHome hooks: Google-feed
  prop, minus-screen reroute, widget-picker reroute, `art_resolver.cpp` = ART
  symbol resolver, `obfstr.h` + `gen_obf_strings.py` = build-time XOR string
  encoding)
- `zygisk-src/test/` — host-side mock test (no device needed); `test/hooker/` —
  JVM regression test for the `jrc.homefeed` hookers (hand-rolled Android stubs
  + fake hook-target classes with the same shapes as the pinned CN build)
- `scripts/build_zygisk.sh` — native build (needs `NDK_BUILD=/path/to/ndk-build`)
- `dualwake_boot.sh` — dual-wake cold-boot watchdog, copied by `service.sh` to
  `/data/local/tmp` and run in the background (see "Dual-wake boot race" below)
- `mount_scrub.sh` — sensitive-process mount-namespace scrubber, same
  `/data/local/tmp` background-worker pattern (see "Sensitive-process mount
  scrub" below)
- `scripts/test_dualwake_boot.sh` — host-side shell test for the watchdog
- `scripts/test_mount_scrub.sh` — host-side shell test for the scrubber
- `build.sh` — packs `dist/HyperOS3_EU_XiaoAI_Portal_MiPay_<version>.zip`
- `customize.sh`, `tools/unity_install.sh` — install-time logic on device

## Build & test

**Always run the mock test before building a zip — no exceptions.**

```sh
sh zygisk-src/test/run_test.sh                                 # regression gate
sh zygisk-src/test/run_hooker_test.sh                          # hooker JVM gate
sh scripts/test_dualwake_boot.sh                               # boot watchdog gate
sh scripts/test_mount_scrub.sh                                 # mount scrub gate
NDK_BUILD="$HOME/Library/Android/sdk/ndk/<ver>/ndk-build" \
    sh scripts/build_zygisk.sh                                 # native .so
sh build.sh                                                    # release zip
```

`build.sh` packages whatever `zygisk/arm64-v8a.so` is present — if native
sources changed, rebuild the .so first, then test, then pack.

Stealth hardening in the native build (the .so stays resident — and readable —
in every non-excluded app's `/proc/self/maps`): `build_zygisk.sh` compiles the
hooker dex with `d8 --release` and strips the final `.so`
(`llvm-strip --strip-all --remove-section=.gnu_debugdata`). The vendored
`libdobby.a` is built from `vendor/dobby/src` with `-DLOGGING_DISABLE` —
without it dobby's `ERROR_LOG`/`FATAL` bake the host's absolute `__FILE__`
paths (incl. the macOS username) into the `.so`. Rebuild it with:

```sh
cmake -S zygisk-src/vendor/dobby/src -B zygisk-src/vendor/dobby/build \
    -DCMAKE_TOOLCHAIN_FILE="$HOME/Library/Android/sdk/ndk/<ver>/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release -DDOBBY_GENERATE_SHARED=OFF \
    -DCMAKE_CXX_FLAGS=-DLOGGING_DISABLE
cmake --build zygisk-src/vendor/dobby/build --target dobby -j8
cp zygisk-src/vendor/dobby/build/libdobby.a zygisk-src/vendor/dobby/lib/
```

(Residual `-g` DWARF in the `.a` is removed by the final strip; the
`.gnu_debugdata` string still visible in `strings` output is
`art_resolver.cpp`'s own section-name constant, not a leftover section.)

Two more stealth layers in the same vein:

- All logging is compiled out of the release `.so`: every `LOGI`/`LOGW` in
  `main.cpp`/`dualwake.cpp`/`homefeed.cpp` sits behind `TAPLUS_DEBUG_LOG`
  (the mock test defines it, so log assertions keep working there). The
  on-device debug flag file therefore has no effect in release builds.
- Functional strings (module paths, sensitive/financial package names, hook
  target class names) never appear as plaintext in any translation unit:
  `gen_obf_strings.py` XOR-encodes them into `gen/obf_strings.h`, invoked by
  both `build_zygisk.sh` and `test/run_test.sh`; native code decodes to a
  stack buffer and wipes it (`obfstr.h`). The generated arrays are
  `const volatile` — without it `-O2` constant-folds the XOR back into a
  plaintext constant pool in `.rodata` (an earlier constexpr-constructor
  attempt failed the same way: clang still emitted the literal pieces).
  Residual readable strings are the JNI/dlsym constants in
  `dualwake.cpp`/`homefeed.cpp` and the embedded hooker dex, which only load
  inside the trusted voice-trigger/launcher processes.

## Zygisk module invariants (do not regress)

- Sensitive processes — `com.google.android.gms` (+`:…`/`.…` children, incl.
  `.unstable`/DroidGuard), `com.google.android.apps.walletnfcrel`,
  `com.android.vending` — must stay **sterile**: no logcat output, no file I/O,
  no `Build` flips. Only `FORCE_DENYLIST_UNMOUNT` + `DLCLOSE_MODULE_LIBRARY`,
  decided first thing in `preAppSpecialize`.
- Financial processes — TW bank/payment prefixes hardcoded in
  `isFinancialProcess` (`main.cpp`; sources: PrivSec-dev Taiwan banking list +
  on-device `pm list packages`, 2026-08) — same first-priority sterile
  treatment as sensitive (no logcat, no file I/O, no flips) **except no
  `FORCE_DENYLIST_UNMOUNT`**, only `DLCLOSE_MODULE_LIBRARY`. These apps' RASP
  flags the "KSU set umounted per app" state itself (mount-namespace
  difference); actively unmounting would create the very evidence they check.
  Umount policy belongs to the KSU/susfs layer, not this module. Never let the
  exclusion list or a flip reach them.
- Logging is compiled out of release builds entirely (`TAPLUS_DEBUG_LOG`);
  the debug flag file
  `/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/debug` is only ever read in
  non-sensitive, non-financial processes (still zygote-privileged there, so
  no app-side trace) and only has an effect in debug-log builds.
- `com.miui.voiceassist:voice_trigger` / `com.miui.voicetrigger*` must never be
  dlclosed — the dual-wake workers live in the module. The exclusion list must
  not override this.
- `com.miui.home` must never be dlclosed **nor flipped** — the homefeed worker
  lives in the module, and the CN launcher's Google minus-screen branch
  (`LauncherAssistantCompat.newInstance`) requires
  `miui.os.Build.IS_INTERNATIONAL_BUILD == true`. Both hold regardless of the
  exclusion list (hardcoded in `main.cpp`).
- The CN MiuiHome payload (`system/product/priv-app/MiuiHome`,
  RELEASE-7.50.06.2529 / vc 750062529) gets its Google feed option from the
  homefeed hook, not from props: CN `DeviceConfig.isUseGoogleMinusScreen()`
  gates on `ro.com.miui.rsa` / `ro.com.miui.rsa.search` carrier allowlists and
  a `{mx_telcel, lm_cr}` region list — all empty on EU. The worker lsplant-hooks
  `android.os.SystemProperties.get(String)` inside the launcher process only
  (reached via the launcher's own reflective
  `com.miui.launcher.utils.SystemProperties` wrapper, so AOT inlining can't
  bypass it) and answers `tier1_5` for `ro.com.miui.rsa`, which sets
  `CAN_SWITCH_MINUS_SCREEN=true`; the actual provider still follows
  `settings system switch_personal_assistant` (`personal_assistant_google` on
  myron), keeping the switchable 資料來源 list like the EU build. Never
  `resetprop` the real prop instead — a global `ro.*` change is visible to
  DroidGuard/Wallet. The Java-side `shouldInstall` pins vc 750062529, so the
  hook stays inert on any other launcher build.
- The bundled CN launcherclient lib reads GSA's `service.api.version` via
  `resolveService(intent, 128)` (EU lib uses 786560 = GET_META_DATA |
  MATCH_DIRECT_BOOT_*) and caches it in a **write-once static** (`…launcherclient
  .LauncherClient.b`): if the resolve fails at boot (GSA was Play-updated minutes
  before; its `cfbv` toggles DrawerOverlayService enabled state by Acetone
  eligibility), the client is stuck on the legacy attach path and GSA never
  sends `service_status` — the minus screen swipe stays dead
  (`mLauncherOverlay:null`) while `isConnected` looks healthy.
  `HomeRsaHooker.ensureServiceApiVersion` re-resolves with the EU flags and
  rewrites the static field, retrying ~5 min to outlast the boot race. GSA-side
  eligibility is device-level (`searchBoxEligible=true`,
  `googleOverlayActive=true`), not launcher-build-specific — verified via
  `dumpsys activity service com.google.android.googlequicksearchbox/com.google
  .android.apps.gsa.nowoverlayservice.DrawerOverlayService` on the working EU
  session (Server version 10, overlay OPENED).
- The CN launcher's 資訊助手 (App Vault) mode is rerouted by
  `MinusScreenHooker`: INTL `newInstance` falls back to binding
  `com.mi.android.globalminusscreen`, which the EU 308 base doesn't ship —
  the hook detects that dead end (reads `mPackageName` up the class chain) and
  substitutes the stock CN pairing
  `LauncherAssistantCompatMIUI(launcher, com.miui.personalassistant)`; the
  installed PersonalAssistant (25.31.01) serves `com.miui.launcher
  .WINDOW_OVERLAY` and the launcher already holds
  `miui.personalassistant.ACCESS_PROVIDER` (signature|privileged). Google mode
  and a real globalminusscreen install pass through untouched. Live source
  switching works because `AssistantLauncherCallbacksWrap.reloadMinusScreen()`
  re-runs `newInstance`.
- The edit-mode 小工具 button (`WidgetManagerUtils.gotoPicker`) is rerouted by
  `WidgetPickerHooker`: `MIUIWidgetUtil.isMIUIWidgetSupport()` chains into
  `MIUIWidgetCompat.sAssistantWidget`, which is chosen by
  `IS_INTERNATIONAL_BUILD` — on EU it is always `AssistantWidgetCompatGlobal`,
  whose `isSupportMIUIWidget` requires `com.mi.globalminusscreen` as a system
  app, so the button always fell back to the built-in AOSP-style list and the
  小部件中心 never opened. The hook re-fires the CN intent verbatim
  (openSource=2 / picker_tip_source / `widget://picker/detail` URI) at the
  installed PersonalAssistant's `picker.business.home.pages.PickerHomeActivity`
  (verified working on-device via `widget://picker/home`). It only engages when
  `isMIUIWidgetSupport()` is false and the device is not P19-low-mem; a working
  stock path (real globalminusscreen) and any startActivity failure both fall
  through to the original method unchanged. Do NOT "fix" this by flipping
  `IS_INTERNATIONAL_BUILD` for com.miui.home — the Google minus-screen branch
  above depends on it staying true.
- Package matching treats `pkg:child` and `pkg.child` as the same package;
  bare prefixes (`com.google.android.gmsx`) must not match.
- ThemeManager region flip must land before the app's first API request:
  `basemodule.utils.DeviceUtils.ld6` (10.8.7.6+; class was `…utils.ld6` before)
  is a **write-once lazy cache** — `DeviceUtils.i()` fills it from
  `miui.os.Build.getRegion()` only while null, and OkHttp `ParamInterceptor`
  reads it into every request's `region=` param. First writer wins, so the
  worker flips it the moment `ActivityThread.currentApplication()` appears
  (before `Application.onCreate` finishes) — never add a head-start sleep
  (the 250ms one made every cold-start home feed go out with the real region
  and render empty until pull-to-refresh).

The mock test covers all of the above — extend it when changing `main.cpp`
decision logic.

## Dual-wake boot race (root cause, revised 2026-08-21 on myron)

The 2026-08-20 "Qualcomm HAL load-order" theory is **retired** — disproven by
a clean same-boot A/B: after an orderly AoHD rebuild GSA's model loaded
*first* (334ms ahead of XiaoAI) and both sides worked, while an identical
GSA-first order after a disorderly `killall audioserver` left Google dead
for 7+ hours. Load order was a red herring; the real variable is whether
GSA's AlwaysOnHotwordDetector chain (system_server `SoundTriggerHelper` ↔
isolated hotword process
`googlequicksearchbox:trusted_disable_art_image_…:…gsa.hotword…` ↔
second-stage audio verification) was rebuilt **cleanly and completely**.

Three confirmed failure modes:

- **Boot wedge**: GSA's first AoHD init can stall in the boot storm
  (measured: ATTACH 21:36:10, no model for 13 minutes, then self-rebuild
  at 21:49:02 and loaded within 100ms). While stalled, Hey Google is dead.
- **No-connection wedge (2026-08-24, live)**: a harsher boot wedge where the
  AoHD stalls **before the HDS bind** — soundtrigger shows only
  ATTACH/GET_MODULE_PROPERTIES (14:09:57), the isolated process never
  appears, `dumpsys voiceinteraction` says `No Hotword detection
  connection`, GSA's own VIS dump says `Sandboxed Detector(s): No detector`.
  It did NOT self-heal in 4.5 hours. Enrollment, eligibility
  (`searchBoxEligible=true`, `googleOverlayActive=true`) and default-VIS
  binding were all intact — purely a wedged init, and there is no isolated
  process to kill. Recovery: kill GSA's **VIS process**
  (`GsaVoiceInteractionService`, `processName` resolved from the
  voiceinteraction dump, currently `:interactor`); system_server rebinds the
  VIS → GSA `onReady` re-creates the AoHD (live-verified 2026-08-24
  18:57:05 kill → 18:57:11 DETACH+re-ATTACH → model loaded 137ms later;
  XiaoAI's ACTIVE model untouched).
- **Stale chain after audioserver kill/crash**: a disorderly HAL teardown
  rebuilds only the middleware sessions; the GSA-side chain survives in a
  half-old state — DSP keeps detecting (middleware logs `RECOGNITION`
  status 0) but events never reach GSA, so the assistant never launches.
  XiaoAI's native `com.miui.voicetrigger` path rebuilds cleanly on its
  own, which is why "小愛 works, Google dead" is the dominant symptom.
  **Never `killall audioserver`** — that is what broke it (self-inflicted
  on 2026-08-20 22:27, broke Google for the rest of that boot).

Fix (what Settings → 預設系統應用程式 → 小幫手與語音助理 toggling does
manually): kill GSA's isolated hotword process; system_server's
HotwordDetectionConnection rebinds within ~5-10s — old session
STOP/UNLOAD/DETACH, fresh attach, fresh model load (new uuid; vendorUuid
stays `7038ddc8-30f2-…`), recognition active. XiaoAI's session is
untouched (live-verified 2026-08-21 07:18). Killing an app process has no
audio impact, so no call-state deferral is needed.

`dualwake_boot.sh` polls every 15s after `boot_completed` (12 rounds max):
re-delivers XiaoAI's BootupReceiver until armed (its model is in
`/data/user/0`, needs unlock), then watches for GSA's model (matched by
`text: X Google` or vendorUuid `7038ddc8-30f2`; only the trailing
active-model listing carries `text:`, detached session logs don't). If GSA
hasn't loaded within `DUALWAKE_GSA_GRACE` (4) rounds, kill the isolated
hotword process; if that process doesn't even exist (no-connection wedge),
kill GSA's VIS process instead — but only when the voiceinteraction dump
shows GSA **is** the default VIS (`processName=com.google.android.
googlequicksearchbox:…`) **and** a keyphrase enrollment exists
(`7038ddc8-30f2`); otherwise Voice Match is genuinely off / another
assistant is default, and it just keeps watching. Both kill types share
the `DUALWAKE_GSA_MAX_FIXES` (2) budget.

The BootupReceiver re-delivery (`am broadcast` — on this build just a shim
over `cmd activity`, so the failure is the native `cmd: Failure calling
service activity: Failed transaction (2147483646)`) can fail **wholesale**
during the boot storm: 2026-08-22 10:28-10:29 measured 3/3 rounds failing
while AMS was overloaded; XiaoAI only survived because it self-armed at
boot+44s. `redeliver_bootup` therefore retries in-round
(`DUALWAKE_RETRY_MAX` 3 attempts, `DUALWAKE_RETRY_INTERVAL` 2s apart) and
logs every attempt's outcome; success is judged by the output containing
`Broadcast completed`, not by rc alone. Host test simulates this with the
`am_fails=N` stub knob (test cases 9-10). Host-test pitfall that hid in
plain sight: macOS bash 3.2 (`/bin/sh`) silently drops bytes when a
`$var` expansion is immediately followed by a multibyte char in a
double-quoted string (`"rc=$rc）"` loses the value and the lead byte of
`）`) — always write `${var}` before CJK/full-width punctuation in worker
log lines, or host-side log assertions go silently wrong.

2026-08-22 post-mortem of the "v1.0.22 broke dual wake" report: **not a
regression**. Middleware/soundtrigger dumps proved both chains armed by
10:29:27 and stayed in the same processes/models all boot; the only proven
dead windows were two phone calls (10:29:39-10:31:51, 10:32:47-10:33:01)
during which SoundTrigger is globally DISABLED by design (GSA
STOP_RECOGNITION, XiaoAI RECOGNITION status 1/abort) — dual wake is
expected to be dead in a call. The single screen-off 小愛 attempt at
11:28:36 fired the DSP but was rejected by XiaoAI's own second-stage
verification in 972ms (no launch, recognition re-armed) — acoustic/
pocket-level rejection, not a chain failure; every attempt from 11:37:31
onward (both sides, screen on) succeeded end-to-end. Nothing in v1.0.22
(MiuiHome payload, homefeed hooks) touches the hotword path.

`service.sh` gates this on `voice_trigger_enabled=1` and XiaoAI not being
the default assistant.

Residual known risk: an audioserver crash *after* the worker's boot window
can still stale the chain silently; there is no cheap external detector
(middleware shows ACTIVE throughout) — recovery is the same kill, done
manually.

## Sensitive-process mount scrub (root cause, 2026-08-24 on myron)

Symptom: Wallet/GMS intermittently prompted "device is rooted"; killing the
app and relaunching always "fixed" it. Live-captured while the prompt was
showing: `com.google.android.gms` / `gms.persistent` mountinfo carried **6**
bind mounts sourced from `/adb/modules/...`, `gms.unstable` (DroidGuard's
home) carried 1, wallet/vending were clean. Maps were sterile everywhere
(our `.so` only lives in `com.miui.home` / `com.miui.voicetrigger`, by
design) — **the leak was never ours**.

Mechanism: KernelSU's per-app "umount modules" runs **once at specialize**
and only covers mounts KSU itself tracks. Third-party module scripts that
hand-roll `mount --bind` (BW_Audio's dolby/quasar XMLs — re-bound by its
watchdog on **every audioserver restart**; the morphe modules bind patched
APKs over `base.apk`) are not tracked, and runtime re-binds **propagate via
the shared mount peer group into still-running long-lived processes**
(gms.persistent/gms essentially never die). DroidGuard reads
`/proc/self/mountinfo`, sees `/adb/modules/...`, flags root. Relaunching the
app re-runs specialize → umount → clean → passes, until the next re-bind
re-dirties it. That is the exact intermittent pattern.

Fix: `mount_scrub.sh` (same `/data/local/tmp` nohup pattern as
dualwake_boot.sh, started unconditionally by `service.sh`) scrubs every 15s:
for each live gms(+`:…`/`.…` children)/wallet/vending process — the same
list as `isSensitiveProcess` in `main.cpp` — it finds mountinfo entries
whose **field 4** (source path within the source fs; `/data/adb/modules`
shows as `/adb/modules`) starts with `/adb/modules/` and `nsenter -t <pid>
-m -- umount -l <mountpoint>` them. Verified live: one round cleaned all 5
processes incl. a Wallet that restarted mid-test; pid 1's mount count never
decreased and audioserver/BW_Audio kept working (only the propagated copies
inside app namespaces are detached — the master binds in the root namespace
are never touched).

Safety invariants (host test `scripts/test_mount_scrub.sh` covers them):

- The pgrep patterns MUST stay anchored (`^com…`): an unanchored
  `pgrep -f` also matches the adb/su shell whose own cmdline contains the
  package names, and nsenter-ing into THAT umounts the **root namespace's
  master binds** (observed first-hand during the manual verification).
- Match only field 4 with an `/adb/modules/` prefix — adbd's functionfs
  (`/dev/usb-ffs/adb`) and KSU/hybrid overlays are different shapes and must
  never be touched.
- toybox `nsenter` eats `umount`'s `-l` as its own option — the `--`
  separator is mandatory.
- Financial processes must NEVER be added to the scrub list: their RASP
  flags the "was umounted" state itself (see the financial invariant above);
  gms/wallet/vending already live with KSU's specialize-time umount, so
  scrubbing changes nothing observable for them.

On a susfs kernel this whole class disappears (`hide_sus_mnts`); myron runs
the official GKI kernel, so the scrubber is the stock-kernel equivalent.

## Deploy

KernelSU (ksud): `ksud module install <zip>` stages into
`/data/adb/modules_update/`; the module activates on reboot only. Never reboot
the user's device without explicit approval.

- Keep "umount modules" OFF for `com.miui.home` in KernelSU Next. Since v1.0.20
  the module deliberately mounts the CN launcher over EU stock; with umount on,
  the launcher process would read the EU stock APK beneath the mounts (the
  homefeed hook then stays inert — version guard — and the CN launcher silently
  reverts to EU). The old reason still applies to any partial-shadowing case:
  a launcher reading mismatched CN/EU string tables misresolves labels (seen on
  SoundRecorder: CN 7.8.9.3 label id 0x7f120048 = EU's
  `another_recording_toast`). ThemeManager only survives because both builds
  share label id 0x7f120126 — alignment luck, not a mechanism.
- `excluded_packages.txt` only drives the Zygisk Taplus/region **flip**
  exclusion (`zygisk-src/main.cpp`); it does NOT exclude systemless mounts.
  Removing a payload APK from `system/product/…` is the only way to un-shadow
  an app. (v1.0.19-era experiment: adding com.miui.home there did not restore
  the EU launcher — the CN APK stayed mounted.)
- Data-app payloads are immune to the split (installed under /data/app, visible
  in every namespace) but only shadow EU stock when CN versionCode ≥ EU's. On
  the EU 308 base, SoundRecorder (708093 < 708099) can never win as a data app
  — it ships systemless since v1.0.11; MediaEditor stayed systemless after
  v1.0.12 even though v1.0.13 bundles CN 204990043 (== EU's vc), because a
  future EU bump would silently flip it back to stock if it were a data app.
- CN updates that redeclare a permission an EU app already owns can never
  install as data apps (`INSTALL_FAILED_DUPLICATE_PERMISSION`, e.g.
  voiceassist 7.13.32 vs EU notes owning
  `com.xiaomi.mihomemanager.permission.receiveBroadcast`). Systemless has no
  such check (system scan tolerates duplicate declarations), so bumping the
  bundled APK in `system/product/app/…` is the fix — v1.0.14 did this for
  VoiceAssistAndroidT 507013032 and ThemeManager 10876.
- GetApps (com.xiaomi.market) can drive CN updates itself, but only as a
  priv-app with `INSTALL_PACKAGES` allowlisted (HyperOS 3 EU defines it
  `signature|privileged`, so `pm grant` can't). The separate `getapps_priv`
  module does this; without it every session dies at
  `STATUS_PENDING_USER_ACTION` (market error 13).
