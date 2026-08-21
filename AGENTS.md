# AGENTS.md

Magisk/KernelSU module: HyperOS 3 EU localization — XiaoAI voice stack, Taplus
(傳送門), Mi Pay chain, ThemeManager region, dual hotword wake.

## Layout

- `system/product/…` — systemless payload APKs (checked by `build.sh`)
- `system/product/etc/permissions/` — priv-app grant XMLs (defensive; the
  xiaomi.eu 308 base already ships these grants)
- `payload/` — data-app APKs installed by the on-device service script
- `zygisk-src/` — Zygisk module (`main.cpp` = Taplus INTL flip + sensitive-process
  policy, `dualwake.cpp` = dual wake, `art_resolver.cpp` = ART symbol resolver)
- `zygisk-src/test/` — host-side mock test (no device needed)
- `scripts/build_zygisk.sh` — native build (needs `NDK_BUILD=/path/to/ndk-build`)
- `dualwake_boot.sh` — dual-wake cold-boot watchdog, copied by `service.sh` to
  `/data/local/tmp` and run in the background (see "Dual-wake boot race" below)
- `scripts/test_dualwake_boot.sh` — host-side shell test for the watchdog
- `build.sh` — packs `dist/HyperOS3_EU_XiaoAI_Portal_MiPay_<version>.zip`
- `customize.sh`, `tools/unity_install.sh` — install-time logic on device

## Build & test

**Always run the mock test before building a zip — no exceptions.**

```sh
sh zygisk-src/test/run_test.sh                                 # regression gate
sh scripts/test_dualwake_boot.sh                               # boot watchdog gate
NDK_BUILD="$HOME/Library/Android/sdk/ndk/<ver>/ndk-build" \
    sh scripts/build_zygisk.sh                                 # native .so
sh build.sh                                                    # release zip
```

`build.sh` packages whatever `zygisk/arm64-v8a.so` is present — if native
sources changed, rebuild the .so first, then test, then pack.

## Zygisk module invariants (do not regress)

- Sensitive processes — `com.google.android.gms` (+`:…`/`.…` children, incl.
  `.unstable`/DroidGuard), `com.google.android.apps.walletnfcrel`,
  `com.android.vending` — must stay **sterile**: no logcat output, no file I/O,
  no `Build` flips. Only `FORCE_DENYLIST_UNMOUNT` + `DLCLOSE_MODULE_LIBRARY`,
  decided first thing in `preAppSpecialize`.
- Logging is off by default everywhere; the debug flag file
  `/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/debug` is only ever read in
  non-sensitive processes (still zygote-privileged there, so no app-side trace).
- `com.miui.voiceassist:voice_trigger` / `com.miui.voicetrigger*` must never be
  dlclosed — the dual-wake workers live in the module. The exclusion list must
  not override this.
- Package matching treats `pkg:child` and `pkg.child` as the same package;
  bare prefixes (`com.google.android.gmsx`) must not match.

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

Two confirmed failure modes:

- **Boot wedge**: GSA's first AoHD init can stall in the boot storm
  (measured: ATTACH 21:36:10, no model for 13 minutes, then self-rebuild
  at 21:49:02 and loaded within 100ms). While stalled, Hey Google is dead.
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
hotword process — at most `DUALWAKE_GSA_MAX_FIXES` (2) times. If no such
process exists (Voice Match off / not bound yet), just keep watching.

`service.sh` gates this on `voice_trigger_enabled=1` and XiaoAI not being
the default assistant.

Residual known risk: an audioserver crash *after* the worker's boot window
can still stale the chain silently; there is no cheap external detector
(middleware shows ACTIVE throughout) — recovery is the same kill, done
manually.

## Deploy

KernelSU (ksud): `ksud module install <zip>` stages into
`/data/adb/modules_update/`; the module activates on reboot only. Never reboot
the user's device without explicit approval.

- Keep "umount modules" OFF for `com.miui.home` in KernelSU Next. With umount
  on, the launcher process reads shadowed APKs from the EU stock files beneath
  the mounts and misresolves labels whenever CN/EU string tables are misaligned
  (seen on SoundRecorder: CN 7.8.9.3 label id 0x7f120048 = EU's
  `another_recording_toast`). ThemeManager only survives because both builds
  share label id 0x7f120126 — alignment luck, not a mechanism.
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
