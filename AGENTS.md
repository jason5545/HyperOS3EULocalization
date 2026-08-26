# AGENTS.md

Magisk/KernelSU module: HyperOS 3 EU localization — XiaoAI voice stack, Taplus
(傳送門), Mi Pay chain, ThemeManager region, dual hotword wake.

## Layout

- `system/product/…` — systemless payload APKs (checked by `build.sh`); APKs
  bundling native code also carry pre-extracted `lib/arm64/` (see "Systemless
  payload native libs" below)
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
  scrub" below); its 15s loop also carries the power-key assistant-follow
  sync (see "Power-key 3s power menu" below)
- `scripts/test_dualwake_boot.sh` — host-side shell test for the watchdog
- `scripts/test_mount_scrub.sh` — host-side shell test for the scrubber and
  the power-key follow sync
- `aicall_defaulton.sh` — AI-call entry default-on one-shot worker, started by
  `service.sh` via the same `/data/local/tmp` pattern (see "AI-call entry gate
  & default-on" below)
- `scripts/test_aicall_defaulton.sh` — its host-side shell test
- `build.sh` — packs `dist/HyperOS3_EU_XiaoAI_Portal_MiPay_<version>.zip`;
  also generates `payload_versions.txt` (aapt2-derived package/versionCode
  manifest driving the `service.sh` registration audit — build artifact,
  not committed)
- `customize.sh`, `tools/unity_install.sh` — install-time logic on device

## Build & test

**Always run the mock test before building a zip — no exceptions.**

```sh
sh zygisk-src/test/run_test.sh                                 # regression gate
sh zygisk-src/test/run_hooker_test.sh                          # hooker JVM gate
sh scripts/test_dualwake_boot.sh                               # boot watchdog gate
sh scripts/test_mount_scrub.sh                                 # mount scrub gate
sh scripts/test_aicall_defaulton.sh                            # AI-call default-on gate
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

## Systemless payload native libs (root cause, 2026-08-26 on myron)

After the 2026-08 factory reset + KSU Next reinstall (base bumped to
OS3.0.309.0.WPMCNXM), every `com.miui.voiceassist` process crash-looped
with `UnsatisfiedLinkError: dlopen failed: library "libmmkv.so" not found`.
The payload APKs ship `extractNativeLibs=true` (or attr absent → default
true) with **deflate-compressed** `lib/arm64-v8a/*.so`, so they cannot
load libs from the APK itself — they rely on PackageManager extracting
them into the prebuilt `<codePath>/lib/arm64/` at scan time. That
silently worked while the systemless mount was a writable magic-mount
tmpfs dir; KSU Next's hybrid mount (`/product/app` etc. = lowerdir-only
overlayfs, `ro`, lowerdir under `/mnt/hm_*`) leaves nowhere to extract,
so every `System.loadLibrary` fails — MMKV is simply the first lib XiaoAI
initializes. (Overlayfs lowerdir edits after mount are not reflected in
the merged view, so there is no live patch — rebuild the zip.)

**Invariant**: every payload APK that bundles `lib/arm64-v8a/*.so` must
ship the complete set pre-extracted at `<payload>/lib/arm64/` (the
pattern UPTsmService already used; its dir additionally carries 3 libs
not present in the APK — keep them). Applies to all 13 payloads incl.
the `extractNativeLibs=false` ones (MIUIAiasstService / ThemeManager /
MIUIContentExtension / MiuiHome): a uniform rule beats per-APK manifest
checks, and the dir is ignored wherever the APK loads libs from itself.
`build.sh` hard-fails on any bundled `.so` missing from `lib/arm64/`.

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

`service.sh` gates this on `voice_trigger_enabled` not being `"0"` (explicit
off) and XiaoAI not being the default assistant — since v1.0.31 a null value
(post-reset / post-migration) no longer silences the worker, and the worker
watches the GSA chain even when XiaoAI never arms (see "ReSukiSU / susfs
migration").

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

## Power-key 3s power menu (root cause, 2026-08-26 on myron)

Symptom: long-press power wakes XiaoAI, but holding past 3s never shows the
power menu. Logcat shows XiaoAI's own part working end-to-end
(`VA_PowerKeyProcessor` KEY_EVENT_DOWN → +~700ms `VA_ShutdownViewManager
addShutdownView` + `VA_CircleProgressbar` 1950ms → +2650ms
`closeShutdownRunnable` removes it; +9.75s would show a reboot layer, +12s
force reboot). **That ring is only a countdown animation — XiaoAI never draws
a power menu.** The menu itself is the framework's very-long-press path:
`PowerKeyRule.onMiuiVeryLongPress` falls through to AOSP
`PhoneWindowManager`, which follows `mVeryLongPressOnPowerBehavior`, driven
by `Settings.Global.power_button_very_long_press`.

MIUI writes that global from `MiuiShortcutTriggerHelper
.setVeryLongPressPowerBehavior()` → `getVeryLongPressPowerBehavior()`: setup
complete + `long_press_power_key` non-empty + `shouldShowPowerPanel()`. On
the xiaomi.eu myron base, `Build.IS_INTERNATIONAL_BUILD=true` but
`IS_GLOBAL_BUILD=false`, so `shouldShowPowerPanel()` takes the CN branch
(`mSupportXiaoaiLongpressPowerKeyTalk && supportXiaoaiLongPressPowerKeyTalk`)
which is always false on INTL, then the `mShouldShowPowerPanel == -1`
fallback reads v1 key `global_power_guide` (absent → default 1) → computes
0 → writes `should_launch_global_power_panel=0` → very-long-press NOTHING.
(`InputFeature.supportGlobalPowerGuide_V2()` reads
`com.miui.securitycore` meta-data `miuiShortCutVersion` — it IS 5 on
xiaomi.eu's SecurityCoreAdd, so the RSA-region branch with the
`should_launch_global_power_panel` observer is active; the global branch of
`shouldShowPowerPanel` just never gets used because `IS_GLOBAL_BUILD` is
false.)

Fix (live-verified 2026-08-26, menu appears at 3s hold):
`settings put system should_launch_global_power_panel 1` — the framework
observer instantly recomputes and writes `power_button_very_long_press=1`.
`service.sh` re-ensures it every boot, plus `global_power_guide 0` as
insurance for the `== -1` init path (absent/default-1 would compute the
panel off). Both writes are inert on real CN builds (different observer key,
CN branch returns earlier). Power+VolUp (`POWER_VOLUME_UP_BEHAVIOR_
GLOBAL_ACTIONS`) always works as a fallback regardless of these settings.

Related gotcha (same code path): the long-press function itself is chosen by
`Settings.System.long_press_power_key`. `launch_voice_assistant` is
**hardcoded to XiaoAI** — `ShortCutActionsUtils.launchVoiceAssistant()` does
`intent.setPackage("com.miui.voiceassist")` with the component from a
framework-res string, ignoring the user's default-assistant choice (Google).
`launch_google_search` instead goes through `launchAssistAction()` → the
default assistant. Its direct path requires `isSupportRsa()` false, which
holds only while `global_power_guide_v2` stays 0 — if anything sets it to 1,
the "power guide" branch fires and (because `IS_GLOBAL_BUILD` is false)
launches XiaoAI with a guide extra instead. Toggling 電源鍵喚醒 inside the
XiaoAI app writes `long_press_power_launch_xiaoai=1`, and the framework
observer then force-rewrites `long_press_power_key` back to
`launch_voice_assistant` on this base — so a Google power-key choice must
leave XiaoAI's in-app power-key toggle off.

Follow sync (v1.0.30): since the framework never links the two settings, the
`mount_scrub.sh` worker's 15s loop reads `Settings.Secure.assistant` and
rewrites `long_press_power_key` to match — GSA → `launch_google_search`,
XiaoAI → `launch_voice_assistant` — so switching assistant in 小幫手與語音助理
auto-switches the power key (and reverts the XiaoAI-toggle grab within one
round). It only touches the two assistant functions; a deliberate
`none`/other power-key assignment is respected. Disable with
`MOUNT_SCRUB_POWER_KEY_SYNC=0`. Live-verified: flip to
`launch_voice_assistant` is corrected to `launch_google_search` within one
round. Host cases 8-13 in `scripts/test_mount_scrub.sh` cover the mapping,
the no-op/respect/off paths.

## AI-call entry gate & default-on (root cause, 2026-08-26 on myron)

Symptom: after the 2026-08 factory reset the 「AI 通話」 entry vanished from
both the dialer ⋮ menu and the in-call UI, and flipping the toggle in the
settings page did nothing.

Gate chain (Contacts.apk `DialerMenuDialog.O1`): the menu item exists only
when `SystemCompat.i() && A() && p()` all pass —

- `i()` — `resolveActivity({pkg=com.xiaomi.aiasst.service, action=
  com.xiaomi.aiasst.service.contact.aicalllog_detail}, MATCH_DEFAULT_ONLY)`
  → resolves to `CallLogDetailActivity`.
- `p()` — MIUIAiasstService manifest meta-data `support_incallui_place` ≥ 1
  (it is 8).
- `A()` — calls `content://com.xiaomi.aiasst.service.aicall.provider` method
  `GET_AICALL_AVAILABLE`; needs `KEY_STATUS_CODE` ∈ {1,3,4}.

Provider-side status — **the provider dispatches by method**: the jadx
switch sends `GET_AICALL_AVAILABLE` to `p033g2.a.b()` and only
`GET_INCALL_VOICE_SETTINGS` to `AICallProvider.b()`. The first diagnosis
(status = `incallctrlbutton`) read the wrong branch — writing that key
alone does NOT bring the ⋮-menu entry back (live-verified).

`a.b()` (the real ⋮-menu gate): `e()` (`l0.g` mode) → 4 (also allowed);
`f()` (focus mode) → 5; **`h()` → 1**; otherwise → 2 (hidden). `h()` is
`SettingsSp.getAIcallStatus(h.s().h("ai_call_callscreen"))` = boolean
`aicall_onoff` in shared_prefs `setting.xml`, default from the
`ai_call_callscreen` cloud key — which EU never receives → **default false
→ status 2 → entry hidden**. That was the whole bug.

`SettingsSp` keys, all in shared_prefs `setting.xml`
(`getSharedPreferences("setting", 0)`):

- `aicall_onoff` (`AICALL_ON`) — master switch, the ⋮-menu gate above
  (cloud `ai_call_callscreen`).
- `callscreen_onoff` (`AICALLSCREEN_ON_INTERIOR`) — in-call entry
  (cloud `ai_call_callscreen_entrance`, also absent on EU).
- `incallctrlbutton` (`INCALLCTRLBUTTON`) — in-call voice-control button;
  this is what `AICallProvider.b()` (the `GET_INCALL_VOICE_SETTINGS`
  branch: 6 when `AbstractC0709u.q()` fails / 2 when
  `ro.miui.ui.version.code` < 10 / else `getInCallCtrlButton ? 1 : 0`)
  actually drives.
- `privacy` — AI-call privacy CTA consent (`BaseActivity.c0/e0` →
  `putPrivacy(true)`; `AbstractC0709u.q()` caches true only). Whether it
  independently gates `GET_AICALL_AVAILABLE` was never isolated — the
  verified-working configuration sets all four, and consent is required
  elsewhere in the flow (the status-6 branch, the settings page). The CTA
  dialog can be summoned manually with
  `am start -n com.xiaomi.aiasst.service/.aicall.activities.CtaDialogActivity`
  (started directly its callback is null, so only `privacy` gets written,
  not `callscreen_onoff`).

Three live-verified pitfalls (all hit first-hand):

- The settings toggle does NOT persist without the overlay permission:
  `InCallCtrlSettingFragment`'s handler checks `BaseActivity.q0()` (=
  `Settings.canDrawOverlays`); when false it only pops the permission
  dialog and returns without writing the pref.
- **Never `am force-stop com.xiaomi.aiasst.service`**: force-stop marks the
  package stopped, and `resolveActivity` then filters it out → `i()` fails
  → entry disappears even with the pref true. Provider attach is NOT
  affected by the stopped state (contacts still starts the process), which
  makes the symptom extra confusing (provider answers, menu still hides).
  To make the app re-read prefs, `kill` the process instead — plain kill
  does not set the stopped flag.
- Probing the provider as root/shell (`content call`) is a red herring:
  the caller check (`getPackagesForUid(0)` → null) returns null, and even
  `su 1001` dies on `ACCESS_CONTENT_PROVIDERS_EXTERNALLY`. Only uid
  1000/1001 and `com.android.{server.telecom,incallui,contacts,phone}` /
  `com.xiaomi.{aiasst.service,phone}` with a matching system signature are
  served. Judge status from the dialer's own log (`SystemCompat: status
  code:N` via `Logger.f`, not logcat) or the menu itself.

Fix (v1.0.33): `aicall_defaulton.sh`, a one-shot worker started by
`service.sh` (same `/data/local/tmp` nohup pattern as dualwake), forces all
four keys to `true` every boot — user asked for unconditional default-on,
so an explicit `false` is flipped back too (v1.0.32 wrote only absent keys,
and only `incallctrlbutton`, which targeted the wrong branch). Mechanics:
tmp-file → chown/chmod (dir owner, 0660) → `mv` (atomic rename; **never
`sed -i`** — toybox sed -i creates a new root-owned inode the app cannot
read); appops `SYSTEM_ALERT_WINDOW` → `allow` unconditionally (same
default-on policy); kills the app process afterwards only when a write
actually happened and it is running (the all-true path exits early without
touching the process). Host test: `scripts/test_aicall_defaulton.sh`
(39 checks). The old `action.sh` (a shortcut that only opened that
settings page) was removed in v1.0.32 — default-on made it pointless.



User migrated from KSU Next to **ReSukiSU (ksud 4.2.0-rc1) + LunarKernel V1.3
(susfs v2.2.0)**, same OS3.0.309.0.WPMCNXM base. Symptom report: "album still
4.3, pic edit FC, soundrec FC" — plus (found during diagnosis) homefeed hooks
inert and dual wake fully dead. Four independent breakages, one shared origin:
mounting and package registration behave differently here. All fixed or
self-healing in v1.0.31; details below.

### 1. Module mounting is delegated to the `hybrid_mount` metamodule

ReSukiSU core does **not** mount modules itself; without the `hybrid_mount`
metamodule (or with a broken one) the entire `system/product/…` payload is
simply absent — `pm path com.miui.voiceassist` empty, EU stock everywhere.
hybrid_mount **6.0.0 is broken for us, twice**:

- Its shipped `config.toml` carries a `[kasumi]` section the 6.0.0 binary
  rejects ("unknown field `kasumi`") → config load fails → defaults.
- Default `overlay_mode=ext4` stages a **shared overlay tree** by copying all
  module files into a fixed-size ext4 image
  (`/data/adb/hybrid-mount/modules.img`); our ~962 MB module overflows it →
  `stage shared overlay tree: No space left on device` → **all** overlay
  modules unmounted for the whole boot (an earlier boot failed the same batch
  on the adreno module's missing `/vendor/gpu` target instead).

hybrid_mount **4.2.0-1815 works** (`mount_errors=0, active_mounts=odm,product,
system,vendor`). Stay on it; do not "upgrade" to 6.0.0 without re-testing.
Side effect of the staged copy: served files carry early-boot-clock mtimes
(e.g. `1970-01-11 11:41`) instead of the module-dir real dates KSU Next's
bind/lowerdir mounts used to show.

### 2. PM 嚴格升級保留 (the FC / stale-version mechanism)

PackageManager rewrites a system-app registration **only when the newly
scanned versionCode is strictly greater than the registered one** (or no
registration exists). Verified end-to-end on-device:

- The migration boot(s) ran with no/broken mounts → PM registered **EU stock**
  (lastUpdateTime `1970-01-10 20:29:53` = that boot's pre-timesync clock):
  MiuiHome 750062545, SoundRecorder 708099, MediaEditor 204990043**-global**,
  ThemeManager 10952, AiAsstVision 540120660.
- Mounts restored → PM logs `changed; collecting certs` for the three payload
  APKs but **keeps the EU registration** (CN vc is lower or equal).
  Result: EU manifest + CN code — SoundRecorder FC
  (`…backup.CloudBackupProvider` CNFE), MediaEditor FC (`FirebaseInitProvider`
  CNFE), MiuiHome registered 2545 while running 2529 code (homefeed version
  gate read the registry → inert). ThemeManager/AiAsstVision are stale the
  same way but non-fatal so far (region flip is native-side, unaffected).
- CN-only payloads (voiceassist 507013032, voicetrigger 2026051416, MiPay
  chain, contentextension) registered correctly at the first rescan that saw
  them — consistent with the same rule (no prior registration).
- It worked for a year on KSU Next because the first post-reset boot always
  had mounts up (CN registered into an empty packages.xml) and payload bumps
  were always strict vc increases. The rule only bites when a boot registers
  EU stock **over** existing CN registrations — i.e. exactly a migration /
  failed-mount boot on the 309 base, whose EU vcs exceed our pins.
- `packages.xml` is **ABX binary XML** (magic `ABX`) — no shell surgery.
  CorePatch is **not** involved (verified in the fork source: its only
  downgrade hook is install-flow `checkDowngrade(PackageInfoLite)`, which the
  boot scan never calls; payload and EU stock share the same `c9009d01`
  platform signature, so the replacement path never hits a verify-failure
  hook).

### 3. What v1.0.31 ships about it

- `payload_versions.txt` — build-time generated (`build.sh`, via aapt2;
  hard-fails without it) manifest of `package versionCode module-rel-path`
  for every payload APK. Single source of truth; kills the hardcoded-vc drift
  class (service.sh expected gallery 5000507 while the shipped APK was
  already 5000712).
- `service.sh` registration audit every boot: registration ≠ shipped vc →
  auto **data-shadow** when CN vc ≥ stuck registration and the payload is not
  a priv-app (priv-apps would lose privileged grants as data apps) —
  MediaEditor (204990043 == 204990043) is fixed this way; everything else
  gets a loud `STALE:` line in `data_app_install.log`. Convergence paths for
  the rest: GetApps CN update with vc > EU 309 stock (its data install
  shadows cleanly), a payload bump in this repo (then re-verify hook shapes
  and re-pin), or the next OTA (`mIsUpgrade` full rescan). On ROM change
  (SystemVersion marker) the shadows are uninstalled first so the post-OTA
  rescan re-registers systemless.
- `ensure_data_app` retries `pm install` in-round (3 × 2s, judged by the
  `Success` string) — the boot-storm `Failed transaction (2147483646)` cost
  Gallery a whole boot ("album still 4.3").
- Dual-wake gate relaxed to `voice_trigger_enabled != "0"` — after a reset /
  migration the setting is **null**, and the old `== "1"` gate silently
  killed the worker (both wakes dead). `dualwake_boot.sh` now also watches
  GSA when XiaoAI never arms (shared `fix_gsa_chain` + shared kill budget) —
  otherwise Hey Google boot wedges have no watcher in exactly that scenario.
  Host cases 1/10 updated, 14-16 added (55 checks total).
- `HomeRsaHooker.shouldInstall` now versions the **running APK**
  (`getPackageArchiveInfo(getApplicationInfo().sourceDir)`) instead of the PM
  registry — the pin protects against unknown code shapes, so it must bind to
  the code, not to a possibly-stale registration. Homefeed works even while
  PM still says 2545. The equivalent pins elsewhere (VoiceTrigger 2026051416)
  are CN-only packages with no EU counterpart and were not touched.

### 4. susfs does **not** obsolete the mount scrubber

susfs (`hide_sus_mnts`) hides KSU's own mounts, but not hand-rolled runtime
binds: BW_Audio's watchdog re-binds were observed leaking into
`gms.persistent` on this very setup (scrubbed 14:06, same as before). Keep
`mount_scrub.sh` unconditional. Zygisk is provided by the Zygisk Next module
(`zygisksu`); it loads our module fine (dmesg), but remaps module memory
anonymous — `/proc/<pid>/maps` no longer shows the module path, so a maps
grep is not a load check under ZN (use behavior/logcat instead).

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
