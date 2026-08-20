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

MockJniKnobs g_jni;

// ---- the module under test ---------------------------------------------------

#include "../main.cpp"

// dualwake stubs (signatures from dualwake.h, pulled in by main.cpp)
void dualwakePreloadLsplant() { g_lsplant_preloads++; }
void dualwakeStartCoreAlive(JavaVM *) { g_core_alive_starts++; }
void dualwakeStartVoiceTrigger(JavaVM *) { g_voice_trigger_starts++; }

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

static void testNonSensitiveNoDebug() {
    g_case = "non-sensitive, no debug flag";
    resetState();
    specialize("com.miui.home");
    CHECK(g_options.empty());                         // not excluded -> no dlclose
    CHECK(g_accessed_paths.size() == 1);              // debug flag probed once
    CHECK(g_accessed_paths[0].find("/debug") != std::string::npos);
    CHECK(g_logs.empty());                            // flag absent -> silent
}

static void testNonSensitiveDebugFlip() {
    g_case = "non-sensitive, debug on, flip logs";
    resetState();
    g_access_result = 0;            // debug flag "exists"
    g_jni.find_class_ok = true;     // miui.os.Build present
    g_jni.static_bool_value = true; // IS_INTERNATIONAL_BUILD currently true
    specialize("com.miui.home");
    CHECK(g_jni.set_static_bool_calls == 1);
    CHECK(g_jni.last_set_static_bool == JNI_FALSE);
    CHECK(logsContain("IS_INTERNATIONAL_BUILD -> false"));
}

static void testSensitivePrefixBoundary() {
    g_case = "gms prefix boundary";
    resetState();
    specialize("com.google.android.gmsx");  // must NOT match
    CHECK(g_options.empty());
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
    testNonSensitiveNoDebug();
    testSensitivePrefixBoundary();
    testExclusions();
    testVoiceTrigger();
    testNonSensitiveDebugFlip();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
