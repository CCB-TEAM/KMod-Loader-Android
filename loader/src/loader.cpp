// PoC: proves the library gets loaded into the game process on Android.
#include <android/log.h>
#include <jni.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <string>
#include <algorithm>

#define TAG "KModLoader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static void Describe(const char* where)
{
    char cmd[256] = {};
    if (FILE* f = fopen("/proc/self/cmdline", "r")) { fread(cmd, 1, sizeof(cmd) - 1, f); fclose(f); }
    LOGI("%s: pid=%d process=%s", where, (int)getpid(), cmd);
}

// The process name is the app's package name (secondary processes have a ":suffix"). Nothing here hardcodes the package.
static const char* ProcessName()
{
    static char cmd[256];
    if (!cmd[0])
        if (FILE* f = fopen("/proc/self/cmdline", "r")) { fread(cmd, 1, sizeof(cmd) - 1, f); fclose(f); }
    return cmd;
}

static bool IsMainProcess()
{
    const char* cmd = ProcessName();
    return cmd[0] != 0 && strchr(cmd, ':') == nullptr;
}

static bool CopyFile(const char* from, const char* to)
{
    FILE* in = fopen(from, "rb"); if (!in) return false;
    FILE* out = fopen(to, "wb"); if (!out) { fclose(in); return false; }
    char buf[1 << 16]; size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in); fclose(out); return ok;
}

#ifdef KMOD_DEV
// DEV build only: if <root>/kmod/dev/libkmodcore.so (Android/media/<pkg>/kmod/dev/) exists, copy it into the private dir and load that
// (so core changes need only an adb push + restart, not a 1.2 GB reinstall). Otherwise load the bundled core.
static void LoadCore()
{
    const char* pkg = ProcessName();
    char src[256], dir[256], dst[300];
    snprintf(src, sizeof(src), "/sdcard/Android/media/%s/kmod/dev/libkmodcore.so", pkg);
    snprintf(dir, sizeof(dir), "/data/data/%s/files/kmod", pkg);
    snprintf(dst, sizeof(dst), "%s/libkmodcore.so", dir);

    if (access(src, R_OK) == 0)
    {
        char parent[256]; snprintf(parent, sizeof(parent), "/data/data/%s/files", pkg);
        mkdir(parent, 0700); mkdir(dir, 0700);
        bool copied = CopyFile(src, dst);
        LOGI("dev core: copy %s -> %s : %s", src, dst, copied ? "ok" : "FAILED");
        if (copied)
        {
            void* h = dlopen(dst, RTLD_NOW);
            LOGI("dev core: dlopen(%s) = %p %s", dst, h, h ? "" : dlerror());
            if (h) return;
        }
    }
    void* h = dlopen("libkmodcore.so", RTLD_NOW);
    LOGI("bundled core: dlopen(libkmodcore.so) = %p %s", h, h ? "" : dlerror());
}
#else
// RELEASE build: only the core bundled in the APK is ever loaded (no external-dir code loading).
static void LoadCore()
{
    void* h = dlopen("libkmodcore.so", RTLD_NOW);
    LOGI("core: dlopen(libkmodcore.so) = %p %s", h, h ? "" : dlerror());
}
#endif

__attribute__((constructor)) static void OnLoad()
{
    Describe("constructor");
    if (!IsMainProcess()) return;
    LoadCore();
}

#ifdef KMOD_DEV
// ---------------------------------------------------------------------------------------------
// DEV overlay bridge (Java class kmod.Overlay lives in classes2.dex)
//   Java -> native : Overlay.nativeEval(String)  -> queue, drained by the core on the game thread (KMod_PopEval)
//   native -> Java : KMod_OverlayAppend(utf8)     -> Overlay.appendBytes(byte[])  (any thread)
// ---------------------------------------------------------------------------------------------
#include <deque>
#include <mutex>

static JavaVM* g_vm = nullptr;
static jclass g_overlayCls = nullptr;
static jmethodID g_appendMid = nullptr;
static std::mutex g_evalMu;
static std::deque<std::string> g_evalQ;

extern "C" JNIEXPORT void JNICALL Java_kmod_Overlay_nativeEval(JNIEnv* env, jclass, jstring code)
{
    if (!code) return;
    const char* u = env->GetStringUTFChars(code, nullptr);   // modified UTF-8; fine for source code (BMP)
    if (u) { std::lock_guard<std::mutex> g(g_evalMu); g_evalQ.emplace_back(u); env->ReleaseStringUTFChars(code, u); }
}

// returns length copied (>=0) or -1 when the queue is empty
extern "C" __attribute__((visibility("default"))) int KMod_PopEval(char* out, int cap)
{
    if (!out || cap <= 0) return -1;
    std::lock_guard<std::mutex> g(g_evalMu);
    if (g_evalQ.empty()) return -1;
    std::string s = std::move(g_evalQ.front()); g_evalQ.pop_front();
    int n = (int)std::min<size_t>(s.size(), (size_t)cap - 1);
    memcpy(out, s.data(), n); out[n] = 0;
    return n;
}

extern "C" __attribute__((visibility("default"))) void KMod_OverlayAppend(const char* utf8)
{
    if (!g_vm || !g_overlayCls || !g_appendMid || !utf8) return;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    size_t n = strlen(utf8);
    jbyteArray arr = env->NewByteArray((jsize)n);
    if (!arr) return;
    env->SetByteArrayRegion(arr, 0, (jsize)n, reinterpret_cast<const jbyte*>(utf8));
    env->CallStaticVoidMethod(g_overlayCls, g_appendMid, arr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(arr);
}
#endif

// the VM, for the core's Java calls (HTTP): kept in every build
static JavaVM* g_javaVm = nullptr;
extern "C" __attribute__((visibility("default"))) JavaVM* KMod_GetJavaVM() { return g_javaVm; }

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    Describe("JNI_OnLoad");
    g_javaVm = vm;
#ifdef KMOD_DEV
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env)
    {
        jclass c = env->FindClass("kmod/Overlay");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (c)
        {
            g_overlayCls = static_cast<jclass>(env->NewGlobalRef(c));
            g_appendMid = env->GetStaticMethodID(g_overlayCls, "appendBytes", "([B)V");
            jmethodID startMid = env->GetStaticMethodID(g_overlayCls, "start", "()V");
            if (startMid) env->CallStaticVoidMethod(g_overlayCls, startMid);
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
            LOGI("overlay: kmod.Overlay started");
        }
        else LOGI("overlay: class kmod.Overlay not found (classes2.dex missing?)");
    }
#endif
    return JNI_VERSION_1_6;
}
