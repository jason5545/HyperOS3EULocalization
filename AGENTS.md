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
- `build.sh` — packs `dist/HyperOS3_EU_XiaoAI_Portal_MiPay_<version>.zip`
- `customize.sh`, `tools/unity_install.sh` — install-time logic on device

## Build & test

**Always run the mock test before building a zip — no exceptions.**

```sh
sh zygisk-src/test/run_test.sh                                 # regression gate
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
