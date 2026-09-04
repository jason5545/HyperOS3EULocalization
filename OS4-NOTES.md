# OS4 (Android 17) migration research

Research date: 2026-09-04, branch `jason/os4-payload-research`.
Source package: `O-myron-ota_images-v4.0.9-OS4.0.0.21.XPMCNXM-user-17.0`
(community repack by "d2u8qwq" of the official CN fastboot package; content
verified stock — no third-party mods inside). myron = REDMI K90 Pro Max =
POCO F8 Ultra (user runs the TW-branch hardware on xiaomi.eu OS3.0.309).

Working files (NOT committed, gitignored): `.tmp-verify/os4/` —
`super.img` (14G raw, zstd -d from super.img.zst), `lpunpack.py` (minimal
dynamic-partition unpacker; geometry at 0x1000/0x2000, header at 0x3000),
extracted partitions `product_a/system_a/system_ext_a/mi_ext_a/odm_a.img`
(EROFS → `fsck.erofs --extract`), jadx outputs (`jadx_home_os4`,
`jadx_settings_os4`, `jadx_tm_os4`, `jadx_vt_os4`).

## Payload version audit (module pin → OS4 CN)

| package | module (OS3 CN) | OS4 CN | structural change |
|---|---|---|---|
| com.miui.voiceassist | 507013032 (7.13.32) | 508000020 (8.0.20.3917) | app → **priv-app** |
| com.miui.voicetrigger | 2026051416 | 2036072817 (10.0.10.0) | dir renamed `VoiceTrigger1` |
| com.xiaomi.aiasst.vision | 540120420 | 540110240 (5.11.2.40) | **vc regressed** |
| com.xiaomi.aiasst.service | 2721 | 2730 (6.2.0) | |
| com.miui.contentextension | 40019 | 50010 (5.1.0) | |
| com.android.soundrecorder | 708093 | 801040 (8.1.4) | → **data-app** |
| com.miui.nextpay | 1870 | 4005 (26.04.23.4) | |
| com.miui.tsmclient | 1945 | 4070 (26.07.22.400.f) | |
| com.unionpay.tsmservice.mi | 65 | 57 | **vc regressed** |
| com.xiaomi.payment | 2003069 | **ABSENT** | not in product/system/system_ext/mi_ext/odm |
| com.android.thememanager | 10876 | 114514 (10.5.1.1) | branch/obfuscator changed |
| com.miui.mediaeditor | 204990043 | 210993930 (2.10.39.3) | → **data-app** |
| com.miui.home | 750062529 (7.50.06.2529) | 801025436 (8.01.02.5436) | **Flutter+Rust rewrite, 0 dex** |
| com.miui.gallery | 5000712 | 5040126 (5.4.1.26) | **Flutter rewrite, 0 dex**, → data-app |
| com.xiaomi.mibrain.speech | 60 | 65 (1.6.5) | → data-app |
| com.miui.voiceassistoverlay | 2 | still in product/overlay | |

Native-lib invariant still applies: OS4 APKs ship deflate-compressed
`lib/arm64-v8a/*.so` (VoiceAssist 108, MiMediaEditor 40, VoiceTrigger 12,
MIUIAiasstService 10, ThemeManager 7, MiuiHome 4, ContentExtension 2) →
pre-extract `lib/arm64/` per payload as before.

## MiuiHome 8.01 = Flutter UI + Rust core (the big one)

- APK has **zero dex**; `lib/arm64-v8a/`: `libapp.so` (27M Dart AOT),
  `libapp_launcher.so` (20M), `libresources_frb.so` (flutter_rust_bridge),
  `libnative_widget_sdk.so`; manifest `uses-library: hyperos.rustruntime.v5`.
- Launcher overlay reimplemented in Rust:
  `crates/launcher_overlay/google_client/src/aidl/com/google/android/libraries/
  launcherclient/ILauncherOverlay{,Callback}.rs` — the Google-feed AIDL
  protocol fully survives.
- **Decision inputs unchanged** (strings in libapp_launcher.so):
  `ro.com.miui.rsa`, `ro.com.miui.rsa.search`, `ro.com.miui.rsa.feature`,
  `MiuiOsBuild_is_international_build` / `is_global_build`,
  `can_switch_minus_screen`, `is_use_google_minus_screen`,
  `only_use_google_minus_screen`, `is_use_miui_minus_screen`,
  `switch_personal_assistant` (`personal_assistant_google` / `_app_vault`),
  `com.mi.android.globalminusscreen` vs `com.miui.personalassistant` pairing
  incl. all picker activities; `miui.personalassistant.ACCESS_PROVIDER`
  still held.
- Props are read via **libc**: undefined symbols `__system_property_get@LIBC`,
  `__system_property_find`, `__system_property_read_callback` (+ rustruntime
  wrappers `SystemProperties_get{,_bool,_i32,_i64,_string}`).

### Consequence for the homefeed hooks

All three Java hooks (HomeRsaHooker / MinusScreenHooker / WidgetPickerHooker)
are dead on OS4 — no Java classes exist. Replacement plan (likely simpler and
more robust): Dobby-hook `__system_property_get` (+ `…_read_callback`) inside
`com.miui.home` only, answer `tier1_5` for `ro.com.miui.rsa`. No version pin
needed (hooked symbol is libc). Must re-investigate in the Rust layer:
the service.api.version write-once boot race (`resolve_service failed` string
still present), the minus-screen provider reroute, and the widget-picker
reroute equivalents.

## Hook triage for the rest

- **settingshook** — intact: OS4 Settings
  `DefaultCombinedPreferenceController.getCombinedProviderInfos` is
  shape-identical (INTL branch returns full list; CN branch filters to
  `availableProviderServices`/`ctsProviderServices`). No change expected.
- **mmedit RegionHooker** — unaffected (framework-method hook; MediaEditor
  still Java, 3 dex).
- **dualwake VoiceTriggerRestartHooker** — VoiceTriggerService still present
  but obfuscation fully remapped (`p064v0.*`, `p046p0.*`); re-derive the `q.k`
  target and re-pin to vc 2036072817.
- **ThemeManager region flip** — `region=` param still injected by
  `basemodule.network.theme.interceptors.g` → `basemodule.utils.ld6.i()`, but
  `i()` now delegates into heavier obfuscation (`hKnhaBBnpKaBoollBalpo…`);
  re-derive the lazy-cache field. First-writer-wins timing rule still applies.
- **Taplus flip** (miui.os.Build reflection) — app-version independent, fine.

## PM strict-upgrade interaction (OS4 EU base unknown yet)

- vc regressions (`aiasst.vision` 540120420→540110240, UPTsmService 65→57):
  if an OS4 EU base registers newer stock, our payload can never win — check
  at migration time.
- CN OS4 itself moved SoundRecorder/MediaEditor/Gallery/SpeechEngine to
  data-app — the EU stock-vs-payload vc race on migration boots stays the
  same class of problem the `service.sh` registration audit handles.

## Direction decision (2026-09-04)

Stay on **xiaomi.eu** as the base (official TW Global explicitly ruled out by
user — POCO branch has features removed; xiaomi.eu restores many, this module
supplements the rest). CN→INTL reverse direction evaluated and rejected:
both branches still exist in code (launcher Rust, Settings, framework flags
are runtime-prop-derived), but it means redoing xiaomi.eu's whole build-time
kitchen at runtime, losing per-process selectivity, and unverifiable
Wallet/integrity behavior. The ROM author's "解除快速分享 / 解鎖 CN GMS 限制"
claims target CN-ROM residents — irrelevant on an EU base (GMS skeleton +
`GmsCn*Overlay` + 4-package priv-permission whitelist are the CN-side
mechanism; none of it applies to xiaomi.eu).

## Migration checklist when xiaomi.eu OS4 (myron) lands

1. Re-run the `service.sh` registration audit logic against the new EU stock
   vcs (payload_versions.txt regenerates via build.sh).
2. Swap payloads to OS4 versions (table above); rebuild every `lib/arm64/`
   pre-extracted set; note dir renames and priv-app/data-app moves.
3. Re-derive VoiceTrigger + ThemeManager obfuscated targets; re-pin.
4. Implement native launcher prop hook (Dobby, com.miui.home only); keep the
   Java hookers version-gated off on 8.01+.
5. Full host-test suite, then on-device soak (dual wake, Wallet, AI call,
   Taplus, ThemeManager region, minus screen both providers, widget picker).
