// Android-view UI for native mods: panels described by a JSON spec are shown as a Dialog on the game's activity by a small
// Java class (overlay/kmodui/kmod/UiBridge.java) that is embedded here as a dex and loaded with a DexClassLoader, so it needs
// no change to the APK. Events come back on the Android UI thread and are queued; they are delivered on the game thread.
#include <jni.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "kmod.h"
#include "kmod_common.h"
#include "kmod_log.h"
#include "lua_host.h"
#include "ui_bridge_dex.h"

namespace
{
struct PanelCb { KModUiEventFn cb; void* user; };
std::map<int, PanelCb> g_panels;                     // game thread only
int g_nextPanel = 1;
struct Event { int panel; std::string id, value; };
std::deque<Event> g_events;
std::mutex g_mu;

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;
jobject g_appLoader = nullptr;
jmethodID g_show = nullptr, g_set = nullptr, g_close = nullptr, g_pick = nullptr, g_pickFile = nullptr, g_saveFile = nullptr;
int g_nextPick = 1;
bool g_tried = false, g_ok = false;
std::string g_err;

JavaVM* GetVM()
{
    if (g_vm) return g_vm;
    using Fn = jint (*)(JavaVM**, jsize, jsize*);
    if (void* h = dlopen("libkmod.so", RTLD_NOW | RTLD_NOLOAD))
    {
        using Get = JavaVM* (*)();
        if (Get g = reinterpret_cast<Get>(dlsym(h, "KMod_GetJavaVM"))) g_vm = g();
    }
    for (const char* lib : {"libnativehelper.so", "libart.so", "libandroid_runtime.so"})
    {
        if (g_vm) break;
        if (void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD))
            if (Fn f = reinterpret_cast<Fn>(dlsym(h, "JNI_GetCreatedJavaVMs"))) { jsize n = 0; f(&g_vm, 1, &n); }
    }
    return g_vm;
}

JNIEnv* Env()
{
    JavaVM* vm = GetVM();
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    return env;
}

std::string TakeException(JNIEnv* env)
{
    if (!env->ExceptionCheck()) return "";
    env->ExceptionClear();
    return "a Java exception was raised";
}

// UTF-8 -> jstring through UTF-16 (NewStringUTF only takes modified UTF-8: 4-byte characters would break)
jstring MakeString(JNIEnv* env, const std::string& s)
{
    std::u16string u;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        i += len;
        if (cp >= 0x10000) { cp -= 0x10000; u += char16_t(0xD800 + (cp >> 10)); u += char16_t(0xDC00 + (cp & 0x3FF)); }
        else u += char16_t(cp);
    }
    return env->NewString(reinterpret_cast<const jchar*>(u.data()), (jsize)u.size());
}

std::string FromJString(JNIEnv* env, jstring js)
{
    if (!js) return "";
    const jchar* c = env->GetStringChars(js, nullptr);
    jsize n = env->GetStringLength(js);
    std::string out;
    for (jsize i = 0; i < n; i++)
    {
        uint32_t cp = c[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) cp = 0x10000 + ((cp - 0xD800) << 10) + (c[++i] - 0xDC00);
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    env->ReleaseStringChars(js, c);
    return out;
}

void JNICALL OnNativeEvent(JNIEnv* env, jclass, jint panel, jstring id, jstring value)
{
    Event e{(int)panel, FromJString(env, id), FromJString(env, value)};
    std::lock_guard<std::mutex> g(g_mu);
    if (g_events.size() < 4096) g_events.push_back(std::move(e));
}

void JNICALL OnNativeRun(JNIEnv*, jclass, jlong fn, jlong user)
{
    if (fn) reinterpret_cast<void (*)(void*)>(fn)(reinterpret_cast<void*>(user));
}

jmethodID g_curActivity = nullptr, g_runOnUi = nullptr;
jobject g_activityRef = nullptr;

bool EnsureBridge()
{
    if (g_tried) return g_ok;
    g_tried = true;
    JNIEnv* env = Env();
    if (!env) { g_err = "no Java VM"; return false; }

    // the dex goes to a private, read-only file (Android refuses to load writable dex files)
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < kUiBridgeDexSize; i++) h = (h ^ kUiBridgeDex[i]) * 16777619u;
    std::string dir = std::string("/data/data/") + KPackageName() + "/files/kmod";
    mkdir((std::string("/data/data/") + KPackageName() + "/files").c_str(), 0771);
    mkdir(dir.c_str(), 0771);
    char name[64]; snprintf(name, sizeof(name), "/uibridge-%08x.dex", h);
    std::string path = dir + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || (unsigned)st.st_size != kUiBridgeDexSize)
    {
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) { g_err = "cannot write the UI dex"; return false; }
        fwrite(kUiBridgeDex, 1, kUiBridgeDexSize, f);
        fclose(f);
        chmod(tmp.c_str(), 0400);
        unlink(path.c_str());
        rename(tmp.c_str(), path.c_str());
    }

    jclass at = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = at ? env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;") : nullptr;
    jobject app = curApp ? env->CallStaticObjectMethod(at, curApp) : nullptr;
    if (!app) { TakeException(env); g_err = "no application object"; return false; }
    jclass ctxCls = env->FindClass("android/content/Context");
    jobject loader = env->CallObjectMethod(app, env->GetMethodID(ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    g_appLoader = env->NewGlobalRef(loader);

    jclass dcl = env->FindClass("dalvik/system/DexClassLoader");
    jmethodID ctor = env->GetMethodID(dcl, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    jstring jpath = env->NewStringUTF(path.c_str()), jdir = env->NewStringUTF(dir.c_str());
    jobject dex = env->NewObject(dcl, ctor, jpath, jdir, (jstring) nullptr, loader);
    if (!dex) { g_err = "DexClassLoader failed: " + TakeException(env); return false; }
    jmethodID loadClass = env->GetMethodID(env->FindClass("java/lang/ClassLoader"), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring cn = env->NewStringUTF("kmod.UiBridge");
    jclass cls = static_cast<jclass>(env->CallObjectMethod(dex, loadClass, cn));
    if (!cls) { g_err = "cannot load kmod.UiBridge: " + TakeException(env); return false; }
    g_cls = static_cast<jclass>(env->NewGlobalRef(cls));
    JNINativeMethod nm[2] = {
        {const_cast<char*>("nativeEvent"), const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;)V"), reinterpret_cast<void*>(&OnNativeEvent)},
        {const_cast<char*>("nativeRun"), const_cast<char*>("(JJ)V"), reinterpret_cast<void*>(&OnNativeRun)}};
    if (env->RegisterNatives(g_cls, nm, 2) != 0) { g_err = "RegisterNatives failed: " + TakeException(env); return false; }
    g_show = env->GetStaticMethodID(g_cls, "show", "(ILjava/lang/String;)V");
    g_set = env->GetStaticMethodID(g_cls, "set", "(ILjava/lang/String;Ljava/lang/String;)V");
    g_close = env->GetStaticMethodID(g_cls, "close", "(I)V");
    g_curActivity = env->GetStaticMethodID(g_cls, "currentActivity", "()Landroid/app/Activity;");
    g_runOnUi = env->GetStaticMethodID(g_cls, "runOnUi", "(JJ)V");
    g_pick = env->GetStaticMethodID(g_cls, "pickImage", "(ILjava/lang/String;)V");
    g_pickFile = env->GetStaticMethodID(g_cls, "pickFile", "(ILjava/lang/String;Ljava/lang/String;)V");
    g_saveFile = env->GetStaticMethodID(g_cls, "saveFile", "(ILjava/lang/String;Ljava/lang/String;)V");
    if (!g_show || !g_set || !g_close || !g_curActivity || !g_runOnUi) { g_err = "UiBridge methods missing"; TakeException(env); return false; }
    g_ok = true;
    KLog(KLOG_INFO, "native", "native UI bridge ready");
    return true;
}
}   // namespace

int NativeUi_Show(const char* specJson, KModUiEventFn cb, void* user)
{
    if (!specJson || !EnsureBridge()) { if (!g_err.empty()) KLog(KLOG_WARN, "native", "ui_show: %s", g_err.c_str()); return -1; }
    JNIEnv* env = Env();
    int id = g_nextPanel++;
    g_panels[id] = {cb, user};
    jstring spec = MakeString(env, specJson);
    env->CallStaticVoidMethod(g_cls, g_show, (jint)id, spec);
    env->DeleteLocalRef(spec);
    std::string ex = TakeException(env);
    if (!ex.empty()) { g_panels.erase(id); return -1; }
    return id;
}

bool Platform_KeyDown(const char*) { return false; }

bool NativeUi_PickImage(const char* destNoExt, KModUiEventFn cb, void* user)
{
    if (!destNoExt || !cb || !EnsureBridge() || !g_pick) return false;
    JNIEnv* env = Env();
    int token = g_nextPick++;
    g_panels[-token] = {cb, user};
    jstring d = MakeString(env, destNoExt);
    env->CallStaticVoidMethod(g_cls, g_pick, (jint)token, d);
    env->DeleteLocalRef(d);
    if (!TakeException(env).empty()) { g_panels.erase(-token); return false; }
    return true;
}

bool NativeUi_PickFile(const char* destNoExt, const char* exts, KModUiEventFn cb, void* user)
{
    if (!destNoExt || !exts || !cb || !EnsureBridge() || !g_pickFile) return false;
    JNIEnv* env = Env();
    int token = g_nextPick++;
    g_panels[-token] = {cb, user};
    jstring d = MakeString(env, destNoExt), e = MakeString(env, exts);
    env->CallStaticVoidMethod(g_cls, g_pickFile, (jint)token, d, e);
    env->DeleteLocalRef(d); env->DeleteLocalRef(e);
    if (!TakeException(env).empty()) { g_panels.erase(-token); return false; }
    return true;
}

bool NativeUi_SaveFile(const char* src, const char* suggestedName, KModUiEventFn cb, void* user)
{
    if (!src || !suggestedName || !cb || !EnsureBridge() || !g_saveFile) return false;
    JNIEnv* env = Env();
    int token = g_nextPick++;
    g_panels[-token] = {cb, user};
    jstring s = MakeString(env, src), n = MakeString(env, suggestedName);
    env->CallStaticVoidMethod(g_cls, g_saveFile, (jint)token, s, n);
    env->DeleteLocalRef(s); env->DeleteLocalRef(n);
    if (!TakeException(env).empty()) { g_panels.erase(-token); return false; }
    return true;
}

void NativeUi_Set(int panel, const char* key, const char* value)
{
    if (!g_ok || !key || !value) return;
    JNIEnv* env = Env();
    jstring k = MakeString(env, key), v = MakeString(env, value);
    env->CallStaticVoidMethod(g_cls, g_set, (jint)panel, k, v);
    env->DeleteLocalRef(k); env->DeleteLocalRef(v);
    TakeException(env);
}

void NativeUi_Close(int panel)
{
    if (!g_ok) return;
    JNIEnv* env = Env();
    env->CallStaticVoidMethod(g_cls, g_close, (jint)panel);
    TakeException(env);
}

// game thread, every tick: delivers the queued events to the mods' callbacks
void NativeUi_Drain()
{
    if (!g_ok) return;
    std::deque<Event> evs;
    { std::lock_guard<std::mutex> g(g_mu); evs.swap(g_events); }
    for (Event& e : evs)
    {
        auto it = g_panels.find(e.panel);
        if (it == g_panels.end()) continue;
        PanelCb pc = it->second;
        if (e.id == "$close" || e.id == "$error" || e.id == "$picked") g_panels.erase(it);
        if (pc.cb) pc.cb(e.panel, e.id.c_str(), e.value.c_str(), pc.user);
    }
}

void* NativeUi_JavaVM() { return GetVM(); }
void* NativeUi_Env() { return Env(); }
void* NativeUi_AppClassLoader() { EnsureBridge(); return g_appLoader; }

// The game's current activity as a JNI global reference (replaced by the next call; do not delete it), or null.
void* NativeUi_Activity()
{
    if (!EnsureBridge()) return nullptr;
    JNIEnv* env = Env();
    jobject a = env->CallStaticObjectMethod(g_cls, g_curActivity);
    TakeException(env);
    jobject fresh = a ? env->NewGlobalRef(a) : nullptr;
    if (a) env->DeleteLocalRef(a);
    if (g_activityRef) env->DeleteGlobalRef(g_activityRef);
    g_activityRef = fresh;
    return fresh;
}

void NativeUi_RunOnUi(void (*fn)(void*), void* user)
{
    if (!fn || !EnsureBridge()) return;
    JNIEnv* env = Env();
    env->CallStaticVoidMethod(g_cls, g_runOnUi, (jlong)reinterpret_cast<intptr_t>(fn), (jlong)reinterpret_cast<intptr_t>(user));
    TakeException(env);
}
