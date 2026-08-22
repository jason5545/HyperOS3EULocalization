// Minimal host-side JNI mock so the zygisk module's decision logic can be
// compiled and driven on macOS/Linux. NOT binary compatible with real JNI —
// only the surface main.cpp / zygisk.hpp actually use.
#pragma once

#include <cstdint>
#include <sys/types.h>  // dev_t, ino_t (used by zygisk.hpp)

typedef int jint;
typedef uint8_t jboolean;
typedef int64_t jlong;

struct _jobject {};
struct _jclass : _jobject {};
struct _jstring : _jobject {};
struct _jarray : _jobject {};
struct _jintArray : _jarray {};
struct _jobjectArray : _jarray {};

typedef _jobject *jobject;
typedef _jclass *jclass;
typedef _jstring *jstring;
typedef _jarray *jarray;
typedef _jintArray *jintArray;
typedef _jobjectArray *jobjectArray;

struct _jfieldID {};
struct _jmethodID {};
typedef _jfieldID *jfieldID;
typedef _jmethodID *jmethodID;

#define JNI_OK 0
#define JNI_FALSE 0
#define JNI_TRUE 1

typedef struct {
    const char *name;
    const char *signature;
    void *fnPtr;
} JNINativeMethod;

struct JavaVM_;

// ---- test knobs (defined in test_main.cpp) ----
struct MockJniKnobs {
    bool find_class_ok = false;     // FindClass / GetStaticFieldID succeed
    bool static_bool_value = false; // GetStaticBooleanField result
    bool theme_worker_ok = false;   // themeRegionWorker: attach + all lookups resolve
    int set_static_bool_calls = 0;
    jboolean last_set_static_bool = 0;
    int set_static_object_calls = 0; // region flips (SetStaticObjectField)
    int attach_calls = 0;
    int detach_calls = 0;
};
extern MockJniKnobs g_jni;

struct JNIEnv_ {
    int GetJavaVM(JavaVM_ **vm);  // defined in test_main.cpp

    const char *GetStringUTFChars(jstring s, jboolean *) {
        return reinterpret_cast<const char *>(s);
    }
    void ReleaseStringUTFChars(jstring, const char *) {}
    bool ExceptionCheck() { return false; }
    void ExceptionClear() {}
    jstring NewStringUTF(const char *) {
        static _jstring str;
        return g_jni.theme_worker_ok ? &str : nullptr;
    }
    jobject CallObjectMethod(jobject, jmethodID, ...) {
        static _jobject obj;
        return g_jni.theme_worker_ok ? &obj : nullptr;
    }
    void DeleteLocalRef(jobject) {}
    jclass FindClass(const char *) {
        static _jclass cls;
        return g_jni.find_class_ok ? &cls : nullptr;
    }
    jfieldID GetStaticFieldID(jclass, const char *, const char *) {
        static _jfieldID fid;
        return g_jni.find_class_ok ? &fid : nullptr;
    }
    jboolean GetStaticBooleanField(jclass, jfieldID) {
        return g_jni.static_bool_value ? JNI_TRUE : JNI_FALSE;
    }
    void SetStaticBooleanField(jclass, jfieldID, jboolean v) {
        g_jni.set_static_bool_calls++;
        g_jni.last_set_static_bool = v;
    }
    void SetStaticObjectField(jclass, jfieldID, jobject) {
        g_jni.set_static_object_calls++;
    }
    jmethodID GetStaticMethodID(jclass, const char *, const char *) {
        static _jmethodID mid;
        return g_jni.theme_worker_ok ? &mid : nullptr;
    }
    jobject CallStaticObjectMethod(jclass, jmethodID, ...) {
        static _jobject obj;
        return g_jni.theme_worker_ok ? &obj : nullptr;
    }
    jclass GetObjectClass(jobject) {
        static _jclass cls;
        return g_jni.theme_worker_ok ? &cls : nullptr;
    }
    jmethodID GetMethodID(jclass, const char *, const char *) {
        static _jmethodID mid;
        return g_jni.theme_worker_ok ? &mid : nullptr;
    }
};

struct JavaVM_ {
    // Default: return JNI_OK but a null env — the module's worker treats that
    // as failure and exits immediately, keeping threads out of the test
    // process. theme_worker_ok hands out a usable env so the worker runs.
    int AttachCurrentThread(JNIEnv_ **env, void *) {
        g_jni.attach_calls++;
        static JNIEnv_ worker_env;
        *env = g_jni.theme_worker_ok ? &worker_env : nullptr;
        return JNI_OK;
    }
    int DetachCurrentThread() {
        g_jni.detach_calls++;
        return JNI_OK;
    }
};

typedef JNIEnv_ JNIEnv;
typedef JavaVM_ JavaVM;
