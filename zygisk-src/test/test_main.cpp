// Host-side mock test for the TaplusIntlFix zygisk module.
//
// Includes ../main.cpp directly so the anonymous-namespace module class is
// reachable. Device-touching surfaces are intercepted by this executable:
//   - access()              -> recorded, result controlled per test
//   - fopen()               -> excluded_packages.txt redirected to a temp file
//   - __android_log_print() -> recorded (only fires if the debug gate passes)
//   - dualwake*()           -> stubbed counters
//
// Build & run: sh zygisk-src/test/run_test.sh

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>

#include <jni.h>  // mock/jni.h via -I

// ---- interception state (defined before main.cpp so overrides are in scope) --

static std::vector<std::string> g_logs;
static std::vector<std::string> g_accessed_paths;
static int g_access_result = -1;               // what fake access() returns
static std::string g_exclude_override;         // real file to serve as exclude list
static int g_fopen_calls = 0;

int __android_log_print(int, const char *, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_logs.emplace_back(buf);
    return 0;
}

extern "C" int access(const char *path, int) {
    if (path && strstr(path, "/debug")) g_accessed_paths.emplace_back(path);
    return g_access_result;
}

extern "C" FILE *fopen(const char *path, const char *mode) {
    if (path && strstr(path, "excluded_packages.txt")) {
        g_fopen_calls++;
        if (g_exclude_override.empty()) return nullptr;  // "no list file"
        path = g_exclude_override.c_str();
    }
    // real fopen via dlsym so nothing else breaks
    static FILE *(*real_fopen)(const char *, const char *) = [] {
        return (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    }();
    return real_fopen(path, mode);
}

static int g_core_alive_starts = 0;
static int g_voice_trigger_starts = 0;
static int g_lsplant_preloads = 0;
static int g_homefeed_preloads = 0;
static int g_homefeed_starts = 0;
static int g_mmedit_preloads = 0;
static int g_mmedit_starts = 0;
static int g_settingshook_preloads = 0;
static int g_settingshook_starts = 0;

MockJniKnobs g_jni;

// Region-flip timing sentinel: count every usleep the worker takes before the
// flip lands. The ThemeManager region cache is a write-once lazy field, so the
// worker must flip the moment the Application appears — any head-start sleep
// before SetStaticObjectField reopens the cold-start empty-feed race.
static unsigned long g_usleep_before_flip = 0;

extern "C" int usleep(useconds_t us) {
    if (g_jni.set_static_object_calls == 0) g_usleep_before_flip += us;
    return 0;  // don't really sleep: keep the suite fast
}

// ---- the module under test ---------------------------------------------------

#include "../main.cpp"

// dualwake stubs (signatures from dualwake.h, pulled in by main.cpp)
void dualwakePreloadLsplant() { g_lsplant_preloads++; }
void dualwakeStartCoreAlive(JavaVM *) { g_core_alive_starts++; }
void dualwakeStartVoiceTrigger(JavaVM *) { g_voice_trigger_starts++; }

// homefeed stubs (signatures from homefeed.h, pulled in by main.cpp)
void homefeedPreloadLsplant() { g_homefeed_preloads++; }
void homefeedStartMiuiHome(JavaVM *) { g_homefeed_starts++; }

// mmedit stubs (signatures from mmedit.h, pulled in by main.cpp)
void mmeditPreloadLsplant() { g_mmedit_preloads++; }
void mmeditStartEditor(JavaVM *) { g_mmedit_starts++; }

// settingshook stubs (signatures from settingshook.h, pulled in by main.cpp)
void settingshookPreloadLsplant() { g_settingshook_preloads++; }
void settingshookStartSettings(JavaVM *) { g_settingshook_starts++; }

static JavaVM_ g_fake_vm;
int JNIEnv_::GetJavaVM(JavaVM_ **vm) {
    *vm = &g_fake_vm;
    return JNI_OK;
}

// ---- fake zygisk host ---------------------------------------------------------

static std::vector<int> g_options;
static zygisk::internal::module_abi *g_abi = nullptr;

static bool mock_register_module(zygisk::internal::api_table *,
                                 zygisk::internal::module_abi *abi) {
    g_abi = abi;
    return true;
}
static void mock_set_option(void *, zygisk::Option o) {
    g_options.push_back(static_cast<int>(o));
}

static JNIEnv_ g_env;

static void loadModule() {
    g_abi = nullptr;
    // static: the api handle inside the module keeps pointing at this table
    static zygisk::internal::api_table tbl{};
    tbl.registerModule = mock_register_module;
    tbl.setOption = mock_set_option;
    zygisk_module_entry(&tbl, &g_env);
}

static zygisk::AppSpecializeArgs *makeArgs(const char *nice) {
    // reference members need stable lvalues
    static jint uid = 0, gid = 0, runtime_flags = 0, mount_external = 0;
    static jintArray gids = nullptr;
    static jobjectArray rlimits = nullptr;
    static jstring se_info = nullptr, nice_name = nullptr,
                 instruction_set = nullptr, app_data_dir = nullptr;
    nice_name = reinterpret_cast<jstring>(const_cast<char *>(nice));
    static zygisk::AppSpecializeArgs args{
        uid, gid, gids, runtime_flags, rlimits, mount_external,
        se_info, nice_name, instruction_set, app_data_dir,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &args;
}

// ---- test scaffold ------------------------------------------------------------

static int g_failures = 0;
static const char *g_case = "";

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL [%s] line %d: %s\n", g_case, __LINE__, #cond);   \
        }                                                                    \
    } while (0)

static void resetState() {
    g_logs.clear();
    g_accessed_paths.clear();
    g_options.clear();
    g_access_result = -1;
    g_exclude_override.clear();
    g_fopen_calls = 0;
    g_core_alive_starts = g_voice_trigger_starts = g_lsplant_preloads = 0;
    g_homefeed_preloads = g_homefeed_starts = 0;
    g_mmedit_preloads = g_mmedit_starts = 0;
    g_settingshook_preloads = g_settingshook_starts = 0;
    g_jni = MockJniKnobs{};
}

static bool optionsAre(std::initializer_list<int> want) {
    return g_options == std::vector<int>(want);
}

static bool logsContain(const char *needle) {
    for (const auto &l : g_logs)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void specialize(const char *nice) {
    auto *args = makeArgs(nice);
    g_options.clear();
    g_abi->preAppSpecialize(g_abi->impl, args);
    g_abi->postAppSpecialize(g_abi->impl, args);
}

// ---- cases --------------------------------------------------------------------

static void testSensitiveSterile(const char *nice) {
    char label[160];
    snprintf(label, sizeof(label), "sensitive %s", nice);
    g_case = label;
    resetState();
    g_access_result = 0;  // even with the debug flag "present"…
    specialize(nice);
    CHECK(optionsAre({zygisk::FORCE_DENYLIST_UNMOUNT, zygisk::DLCLOSE_MODULE_LIBRARY}));
    CHECK(g_logs.empty());          // …sensitive path must stay silent
    CHECK(g_accessed_paths.empty()); // …and must never touch the debug flag
    CHECK(g_fopen_calls == 0);       // …and never read the exclude list
}

static void testFinancialSterile(const char *nice) {
    char label[160];
    snprintf(label, sizeof(label), "financial %s", nice);
    g_case = label;
    resetState();
    g_access_result = 0;            // even with the debug flag "present"…
    g_jni.find_class_ok = true;     // …and miui.os.Build available…
    g_jni.static_bool_value = true;
    specialize(nice);
    CHECK(optionsAre({zygisk::DLCLOSE_MODULE_LIBRARY}));  // dlclose only —
    // NEVER FORCE_DENYLIST_UNMOUNT: these apps' RASP flags the per-app
    // umount state itself; forcing it would create the evidence they check.
    CHECK(g_logs.empty());           // …silent
    CHECK(g_accessed_paths.empty()); // …never touch the debug flag
    CHECK(g_fopen_calls == 0);       // …never read the exclude list
    CHECK(g_jni.set_static_bool_calls == 0);  // …never flipped
}

static void testNonSensitiveNoDebug() {
    g_case = "non-sensitive, no debug flag";
    resetState();
    specialize("com.example.someapp");
    CHECK(g_options.empty());                         // not excluded -> no dlclose
    CHECK(g_accessed_paths.size() == 1);              // debug flag probed once
    CHECK(g_accessed_paths[0].find("/debug") != std::string::npos);
    CHECK(g_logs.empty());                            // flag absent -> silent
    CHECK(g_jni.attach_calls == 0);                   // no theme worker for other apps
}

static void testNonSensitiveDebugFlip() {
    g_case = "non-sensitive, debug on, flip logs";
    resetState();
    g_access_result = 0;            // debug flag "exists"
    g_jni.find_class_ok = true;     // miui.os.Build present
    g_jni.static_bool_value = true; // IS_INTERNATIONAL_BUILD currently true
    specialize("com.example.someapp");
    CHECK(g_jni.set_static_bool_calls == 1);
    CHECK(g_jni.last_set_static_bool == JNI_FALSE);
    CHECK(logsContain("IS_INTERNATIONAL_BUILD -> false"));
}

static void testMiuiHome() {
    g_case = "miui home: resident, never flipped";
    resetState();
    g_access_result = 0;            // debug flag "exists"
    g_jni.find_class_ok = true;     // miui.os.Build present
    g_jni.static_bool_value = true; // IS_INTERNATIONAL_BUILD currently true
    specialize("com.miui.home");
    CHECK(g_options.empty());                  // stays resident (no dlclose)
    CHECK(g_homefeed_preloads == 1);           // lsplant preloaded in pre
    CHECK(g_homefeed_starts == 1);             // prop-hook worker started in post
    CHECK(g_jni.set_static_bool_calls == 0);   // launcher is NEVER flipped:
                                               // CN minus-screen Google branch
                                               // needs IS_INTERNATIONAL_BUILD=true

    // dotted/colon child processes are the same package
    resetState();
    specialize("com.miui.home:recents");
    CHECK(g_homefeed_starts == 1);
    CHECK(g_jni.set_static_bool_calls == 0);

    // the exclude list must not unload or flip the launcher either
    g_case = "miui home: exclusion cannot unload";
    resetState();
    char tmp[] = "/tmp/taplus_test_exclude_home.XXXXXX";
    int fd = mkstemp(tmp);
    const char *content = "com.miui.home\n";
    write(fd, content, strlen(content));
    close(fd);
    g_exclude_override = tmp;
    g_jni.find_class_ok = true;
    g_jni.static_bool_value = true;
    specialize("com.miui.home");
    CHECK(g_options.empty());
    CHECK(g_homefeed_preloads == 1 && g_homefeed_starts == 1);
    CHECK(g_jni.set_static_bool_calls == 0);
    unlink(tmp);
}

static void testMediaEditor() {
    g_case = "mediaeditor: resident, region hook, still flipped";
    resetState();
    g_access_result = 0;            // debug flag "exists"
    g_jni.find_class_ok = true;     // miui.os.Build present
    g_jni.static_bool_value = true; // IS_INTERNATIONAL_BUILD currently true
    specialize("com.miui.mediaeditor");
    CHECK(g_options.empty());                  // stays resident (no dlclose)
    CHECK(g_mmedit_preloads == 1);             // lsplant preloaded in pre
    CHECK(g_mmedit_starts == 1);               // region-hook worker started in post
    CHECK(g_jni.set_static_bool_calls == 1);   // unlike miui_home, the editor
                                               // keeps the Taplus flip

    // dotted/colon child processes are the same package
    resetState();
    specialize("com.miui.mediaeditor:push");
    CHECK(g_mmedit_starts == 1);

    // bare prefix must NOT match
    g_case = "mediaeditor: prefix boundary";
    resetState();
    specialize("com.miui.mediaeditorx");
    CHECK(g_mmedit_preloads == 0 && g_mmedit_starts == 0);

    // the exclude list must not unload the editor either (worker must stay)
    g_case = "mediaeditor: exclusion cannot unload";
    resetState();
    char tmp[] = "/tmp/taplus_test_exclude_mmedit.XXXXXX";
    int fd = mkstemp(tmp);
    const char *content = "com.miui.mediaeditor\n";
    write(fd, content, strlen(content));
    close(fd);
    g_exclude_override = tmp;
    specialize("com.miui.mediaeditor");
    CHECK(g_options.empty());
    CHECK(g_mmedit_preloads == 1 && g_mmedit_starts == 1);
    unlink(tmp);
}

static void testSettings() {
    g_case = "settings: resident, credman hook, still flipped";
    resetState();
    g_access_result = 0;            // debug flag "exists"
    g_jni.find_class_ok = true;     // miui.os.Build present
    g_jni.static_bool_value = true; // IS_INTERNATIONAL_BUILD currently true
    specialize("com.android.settings");
    CHECK(g_options.empty());                  // stays resident (no dlclose)
    CHECK(g_settingshook_preloads == 1);       // lsplant preloaded in pre
    CHECK(g_settingshook_starts == 1);         // credman hook worker started in post
    CHECK(g_jni.set_static_bool_calls == 1);   // unlike miui_home, Settings keeps
                                               // the Taplus flip — only the
                                               // credentials page is unfiltered

    // child processes are NOT hooked: the credentials page lives in the main process
    resetState();
    specialize("com.android.settings:remote");
    CHECK(g_settingshook_preloads == 0 && g_settingshook_starts == 0);

    // bare prefix must NOT match
    g_case = "settings: prefix boundary";
    resetState();
    specialize("com.android.settingsx");
    CHECK(g_settingshook_preloads == 0 && g_settingshook_starts == 0);

    // the exclude list must not unload Settings either (worker must stay)
    g_case = "settings: exclusion cannot unload";
    resetState();
    char tmp[] = "/tmp/taplus_test_exclude_settings.XXXXXX";
    int fd = mkstemp(tmp);
    const char *content = "com.android.settings\n";
    write(fd, content, strlen(content));
    close(fd);
    g_exclude_override = tmp;
    specialize("com.android.settings");
    CHECK(g_options.empty());
    CHECK(g_settingshook_preloads == 1 && g_settingshook_starts == 1);
    unlink(tmp);
}

static void testSensitivePrefixBoundary() {
    g_case = "gms prefix boundary";
    resetState();
    specialize("com.google.android.gmsx");  // must NOT match
    CHECK(g_options.empty());
}

static void testFinancialPrefixBoundary() {
    g_case = "financial prefix boundary";
    resetState();
    specialize("com.cathaybkx.evil");   // must NOT match the bank prefix…
    CHECK(g_options.empty());
    CHECK(g_accessed_paths.size() == 1);  // …so it takes the normal path
                                          // (debug flag probed once)

    resetState();
    specialize("com.jkosx.app");
    CHECK(g_options.empty());
    CHECK(g_accessed_paths.size() == 1);
}

static void testExclusions() {
    g_case = "exclude list";
    resetState();
    char tmp[] = "/tmp/taplus_test_exclude.XXXXXX";
    int fd = mkstemp(tmp);
    const char *content =
        "# comment line\n"
        "\n"
        "com.foo.bar\n"
        "com.miui.securitycenter\n"
        "com.miui.voiceassist\n";
    write(fd, content, strlen(content));
    close(fd);
    g_exclude_override = tmp;

    specialize("com.foo.bar");
    CHECK(optionsAre({zygisk::DLCLOSE_MODULE_LIBRARY}));  // excluded -> dlclose only
    CHECK(!logsContain("sensitive"));

    specialize("com.foo.barx");  // prefix boundary must not match
    CHECK(g_options.empty());

    specialize("com.miui.securitycenter.remote");  // dotted sub-process matches
    CHECK(optionsAre({zygisk::DLCLOSE_MODULE_LIBRARY}));

    // voice targets are protected: exclusion must not unload the module
    specialize("com.miui.voiceassist:voice_trigger");
    CHECK(g_options.empty());
    CHECK(g_core_alive_starts == 1);

    unlink(tmp);
}

static void testVoiceTrigger() {
    g_case = "voicetrigger dualwake";
    resetState();
    specialize("com.miui.voicetrigger");
    CHECK(g_lsplant_preloads == 1);     // preloaded in preAppSpecialize
    CHECK(g_voice_trigger_starts == 1); // worker started in postAppSpecialize
}

static void testThemeRegionFlipIsImmediate() {
    g_case = "theme region flip lands without head-start sleep";
    resetState();
    g_usleep_before_flip = 0;
    g_jni.find_class_ok = true;   // ActivityThread / ClassLoader / DeviceUtils resolve
    g_jni.theme_worker_ok = true; // attach succeeds, Application + classloader ready
    specialize("com.android.thememanager");

    // The worker runs on its own detached thread (mock usleep is instant, so
    // a healthy run finishes almost immediately). Wait for the flip.
    for (int i = 0; i < 400 && g_jni.set_static_object_calls == 0; ++i) {
        const struct timespec ts = {0, 5 * 1000 * 1000};  // 5ms
        nanosleep(&ts, nullptr);
    }
    for (int i = 0; i < 400 && g_jni.detach_calls == 0; ++i) {
        const struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    CHECK(g_jni.set_static_object_calls == 1);  // flipped on the first attempt
    CHECK(g_usleep_before_flip == 0);           // no head start once app appears
    CHECK(g_jni.attach_calls == 1 && g_jni.detach_calls == 1);
}

int main() {
    loadModule();
    if (!g_abi) {
        printf("FAIL: module did not register\n");
        return 1;
    }

    testSensitiveSterile("com.google.android.apps.walletnfcrel");
    testSensitiveSterile("com.google.android.apps.walletnfcrel:payment");
    testSensitiveSterile("com.google.android.gms");
    testSensitiveSterile("com.google.android.gms.unstable");
    testSensitiveSterile("com.google.android.gms:ui");
    testSensitiveSterile("com.android.vending");
    // financial sterile class: dlclose only, no unmount, no flip, no I/O
    testFinancialSterile("com.cathaybk.mymobibank.android");
    testFinancialSterile("com.cathaybk.mymobibank.android:push");
    testFinancialSterile("com.cathaysec.eservice");
    testFinancialSterile("com.chb.mobile.pmb");
    testFinancialSterile("com.chinatrust.mobilebank");
    testFinancialSterile("com.esunbank.ESUNWALLET");
    testFinancialSterile("tw.com.taishinbank.richart");
    testFinancialSterile("tw.com.taishinbank.mobile:remote");
    testFinancialSterile("tw.com.megabank.mobilebank.pre");
    testFinancialSterile("com.sinopac.ismartstock");
    testFinancialSterile("com.nextbank.ncbportal");
    testFinancialSterile("com.ipass.ipassmoney");
    testFinancialSterile("tw.gov.post.mpost");
    testFinancialSterile("com.jkos.app");
    testFinancialSterile("com.eg.android.AlipayGphone");
    testFinancialSterile("com.mitake.android.epost");
    testNonSensitiveNoDebug();
    testSensitivePrefixBoundary();
    testFinancialPrefixBoundary();
    testExclusions();
    testVoiceTrigger();
    testNonSensitiveDebugFlip();
    testMiuiHome();
    testMediaEditor();
    testSettings();
    testThemeRegionFlipIsImmediate();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
