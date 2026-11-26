// kmod mod core (Android prototype): hooks UObject::ProcessEvent, gets onto the game thread,
// and builds a minimal "Mods" button + panel with UMG widgets created at runtime.
#include <android/log.h>
#include <dirent.h>
#include <strings.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <utility>
#include <cstddef>
#include <map>
#include <unordered_set>
#include <mutex>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "kmod_common.h"
#include "kmod_log.h"
#include "kmod_version.h"
#include "lua_host.h"
#include "resolver.h"
#include "kmod_boot.h"
#include "pak_mods.h"
#include "net_redirect.h"
#include "reflect.h"
#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"

using namespace SDK;

void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    KLogMsg(KLOG_INFO, "core", buf);
}

// ---------------------------------------------------------------------------------------------
// Minimal arm64 inline hook (patches 16 bytes; refuses if the first 4 instructions are PC-relative)
// ---------------------------------------------------------------------------------------------
static bool IsPcRelative(uint32_t i)
{
    if ((i & 0x1F000000) == 0x10000000) return true; // ADR/ADRP
    if ((i & 0x3B000000) == 0x18000000) return true; // LDR literal
    if ((i & 0x7C000000) == 0x14000000) return true; // B/BL
    if ((i & 0xFF000010) == 0x54000000) return true; // B.cond
    if ((i & 0x7E000000) == 0x34000000) return true; // CBZ/CBNZ
    if ((i & 0x7E000000) == 0x36000000) return true; // TBZ/TBNZ
    return false;
}

// Copies the 4 instructions that the hook overwrites into the trampoline. Position-dependent instructions are rewritten so they
// still work at the new address: ADR/ADRP/LDR (literal)/B/BL become an absolute load of the original target. Conditional
// branches (B.cond, CBZ, TBZ ...) in the first 4 instructions are not supported.
static bool RelocatePrologue(const uint32_t* code, uint32_t* tr, size_t& n)
{
    auto loadAbs = [&](int reg, uint64_t abs)
    {
        tr[n++] = 0x58000040u | (uint32_t)reg;       // ldr Xreg, #8
        tr[n++] = 0x14000003u;                       // b +12 (over the literal)
        memcpy(&tr[n], &abs, 8); n += 2;
    };
    for (int i = 0; i < 4; i++)
    {
        uint32_t ins = code[i];
        uint64_t pc = reinterpret_cast<uint64_t>(&code[i]);
        if ((ins & 0x9F000000) == 0x90000000 || (ins & 0x9F000000) == 0x10000000)          // ADRP / ADR
        {
            int64_t imm = (int64_t)((((ins >> 5) & 0x7FFFF) << 2) | ((ins >> 29) & 3));
            if (imm & 0x100000) imm -= 0x200000;
            uint64_t abs = (ins & 0x80000000) ? ((pc & ~0xFFFull) + (uint64_t)(imm << 12)) : (pc + (uint64_t)imm);
            loadAbs(ins & 31, abs);
        }
        else if ((ins & 0x3B000000) == 0x18000000)                                          // LDR (literal)
        {
            if (ins & 0x04000000) { Log("hook: SIMD literal load in the prologue, refusing"); return false; }
            uint32_t opc = ins >> 30;
            if (opc == 3) { Log("hook: PRFM literal in the prologue, refusing"); return false; }
            int64_t off = (int64_t)((ins >> 5) & 0x7FFFF); if (off & 0x40000) off -= 0x80000;
            loadAbs(16, pc + (uint64_t)(off * 4));
            uint32_t rt = ins & 31;
            tr[n++] = (opc == 1 ? 0xF9400000u : opc == 2 ? 0xB9800000u : 0xB9400000u) | (16u << 5) | rt;   // ldr X/W/sw Rt, [x16]
        }
        else if ((ins & 0x7C000000) == 0x14000000)                                          // B / BL
        {
            int64_t off = (int64_t)(ins & 0x3FFFFFF); if (off & 0x2000000) off -= 0x4000000;
            loadAbs(16, pc + (uint64_t)(off * 4));
            tr[n++] = (ins & 0x80000000) ? 0xD63F0200u : 0xD61F0200u;                      // blr x16 / br x16
        }
        else if ((ins & 0xFF000010) == 0x54000000 || (ins & 0x7E000000) == 0x34000000 || (ins & 0x7E000000) == 0x36000000)
        { Log("hook: conditional branch (0x%08x) in the prologue, refusing", ins); return false; }
        else tr[n++] = ins;
    }
    return true;
}

static bool InstallInlineHook(void* target, void* detour, void** outTrampoline)
{
    uint32_t* code = reinterpret_cast<uint32_t*>(target);
    void* mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { Log("hook: trampoline mmap RWX failed errno=%d", errno); return false; }
    uint32_t* tr = reinterpret_cast<uint32_t*>(mem);
    size_t n = 0;
    if (!RelocatePrologue(code, tr, n)) { munmap(mem, 4096); return false; }
    tr[n++] = 0x58000050;     // ldr x16, #8
    tr[n++] = 0xD61F0200;     // br x16
    *reinterpret_cast<uint64_t*>(&tr[n]) = reinterpret_cast<uint64_t>(target) + 16;
    n += 2;
    __builtin___clear_cache(reinterpret_cast<char*>(tr), reinterpret_cast<char*>(tr + n));

    uintptr_t page = reinterpret_cast<uintptr_t>(target) & ~uintptr_t(4095);
    if (mprotect(reinterpret_cast<void*>(page), 8192, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    { Log("hook: mprotect RWX on target failed errno=%d", errno); return false; }

    uint32_t patch[4] = { 0x58000050, 0xD61F0200, 0, 0 };
    *reinterpret_cast<uint64_t*>(&patch[2]) = reinterpret_cast<uint64_t>(detour);
    memcpy(target, patch, 16);
    __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target) + 16);
    mprotect(reinterpret_cast<void*>(page), 8192, PROT_READ | PROT_EXEC);

    *outTrampoline = mem;
    return true;
}

bool KMod_InstallHook(void* target, void* detour, void** original) { return InstallInlineHook(target, detour, original); }

// ---------------------------------------------------------------------------------------------
// ProcessEvent detour -> game-thread tick
// ---------------------------------------------------------------------------------------------
using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
static ProcessEventFn g_origPE = nullptr;
static std::atomic<uint64_t> g_peCalls{0};
static std::atomic<uint64_t> g_peTotal{0};
static std::atomic<bool> g_diag{false};                // collect which functions each path sees (kmod.script_diag)
static std::unordered_set<UFunction*> g_diagPI, g_diagPE;   // game thread only
static std::atomic<uint64_t> g_peScript{0};           // ProcessEvent calls of Blueprint (script) functions            // every ProcessEvent call (statistics)
static thread_local int t_gameThread = -1; // -1 unknown, 0 no, 1 yes
static thread_local bool t_busy = false;

static void OnGameTick();
static UObject* CurrentWorld();
// Heartbeat: an engine timer (K2_SetTimer, every ~16 ms) calls Actor.ForceNetUpdate on the PlayerController. The call goes
// through ProcessEvent, so it drives the tick even on a static screen where the game itself calls almost nothing.
static UFunction* g_hbFn = nullptr;

// ---------------------------------------------------------------------------------------------
// Script (Blueprint) function hooks. A Blueprint function runs through ProcessInternal(Context, Stack, Result): the VM calls
// it directly for Blueprint-to-Blueprint calls (no ProcessEvent), and ProcessEvent reaches it too. By the time it is entered
// the caller has evaluated the arguments into the frame's Locals, so a hook sees the parameters, can change them before the
// function runs, and (post hook) read the return value. Native functions keep going through the ProcessEvent hook.
// FFrame (UE 5.x): FOutputDevice base (16 bytes), Node, Object, Code, Locals.
// ---------------------------------------------------------------------------------------------
// FFrame field offsets. The defaults are UE 5.x's; they are re-learned at run time from the first ProcessEvent calls of Blueprint
// functions: ProcessEvent builds the frame itself, so we know what it must contain (function, object, parameter block, the
// function's bytecode pointer) and look for those values in the frame the interpreter receives.
static int kFrameNode = 0x10, kFrameObject = 0x18, kFrameLocals = 0x28, kCodeOff = 0x20;
static bool g_frameLearned = false;
struct FrameExpect { UFunction* fn = nullptr; UObject* self = nullptr; void* parms = nullptr; uintptr_t script = 0; uint64_t head = 0; bool hasHead = false; };
static thread_local FrameExpect t_expect;
static int g_fvNode[26], g_fvObj[26], g_fvLoc[26], g_fvCode[26], g_fSamples = 0;

static int g_fHeadSamples = 0;

static void LearnFrame(const uint8_t* st)
{
    FrameExpect e = t_expect; t_expect = FrameExpect();
    if (!e.fn || g_frameLearned) return;
    g_fSamples++;
    if (e.hasHead) g_fHeadSamples++;
    for (int k = 0; k < 26; k++)
    {
        uintptr_t w = *reinterpret_cast<const uintptr_t*>(st + k * 8);
        g_fvNode[k] += w == (uintptr_t)e.fn; g_fvObj[k] += w == (uintptr_t)e.self;
        g_fvCode[k] += (e.script && w == e.script);
        // Locals: ProcessEvent copies the parameters into the frame's own block, so look for a pointer to memory that starts with them
        uint64_t first = 0;
        if (e.hasHead && w > 0x10000 && w != (uintptr_t)e.parms && Resolver_SafeRead(w, &first, 8) && first == e.head) g_fvLoc[k]++;
    }
    if (g_fSamples < 60 || (g_fHeadSamples < 40 && g_fSamples < 600)) return;
    auto best = [&](const int* v, int& off) { int b = 0; for (int k = 1; k < 26; k++) if (v[k] > v[b]) b = k; off = b * 8; return v[b]; };
    int n, o, l, c;
    int vn = best(g_fvNode, n), vo = best(g_fvObj, o), vl = best(g_fvLoc, l), vc = best(g_fvCode, c);
    bool core3 = vn * 10 >= g_fSamples * 9 && vo * 10 >= g_fSamples * 9 && n != o;
    bool loc = g_fHeadSamples >= 40 && vl * 10 >= g_fHeadSamples * 8 && l != n && l != o;
    bool code = vc * 10 >= g_fSamples * 8 && c != n && c != o && c != l;
    if (core3) { kFrameNode = n; kFrameObject = o; }
    if (core3 && loc) kFrameLocals = l;
    if (core3 && code) kCodeOff = c;
    g_frameLearned = true;
    KLog(core3 && loc && code ? KLOG_INFO : KLOG_WARN, "core",
         "script hook: FFrame offsets (learned: Node %d, Object %d, Locals %d, Code %d): Node +0x%x Object +0x%x Locals +0x%x Code +0x%x; votes %d/%d/%d/%d of %d (%d with parameters)",
         core3, core3, core3 && loc, core3 && code, kFrameNode, kFrameObject, kFrameLocals, kCodeOff, vn, vo, vl, vc, g_fSamples, g_fHeadSamples);
}

using ProcessInternalFn = void (*)(UObject*, void*, void*);
static ProcessInternalFn g_origPI = nullptr;
static std::atomic<bool> g_scriptHookOk{false};          // installed and the frame layout was confirmed
static std::atomic<int> g_piChecked{0};
static std::atomic<bool> g_piBroken{false};
static std::atomic<uint64_t> g_piTotal{0};            // every script function call that went through the detour

static std::atomic<int> g_frameChain{-1};              // FFrame::PropertyChainForCompiledIn offset (learned from script frames)
static int g_chainSamples = 0, g_chainVotes[14] = {};

static void HookedProcessInternal(UObject* ctx, void* stack, void* result)
{
    g_piTotal.fetch_add(1, std::memory_order_relaxed);
    if (!g_scriptHookOk.load(std::memory_order_relaxed) && g_piBroken.load()) { g_origPI(ctx, stack, result); return; }
    if (t_gameThread == 1 && !t_busy && stack)
    {
        if (t_expect.fn) LearnFrame(reinterpret_cast<const uint8_t*>(stack));
        ctx = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(stack) + kFrameObject);      // the interpreter's own Context
        UFunction* fn = *reinterpret_cast<UFunction**>(reinterpret_cast<uint8_t*>(stack) + kFrameNode);
        if (g_piChecked.load(std::memory_order_relaxed) < 2000)          // confirm the frame layout on the first calls
        {
            UObject* obj = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(stack) + kFrameObject);
            if (obj && fn && Rf::ClassOf(reinterpret_cast<UObject*>(fn)) == Rf::FunctionClass()) g_piChecked++;
            else { g_piBroken = true; g_scriptHookOk = false; KLog(KLOG_ERROR, "core", "script hook: unexpected FFrame layout, switched off"); g_origPI(ctx, stack, result); return; }
            if (g_piChecked.load() >= 2000) KLog(KLOG_INFO, "core", "script hook: frame layout confirmed on 2000 calls");
        }
        if (g_diag.load(std::memory_order_relaxed) && fn && g_diagPI.size() < 20000) g_diagPI.insert(fn);
        if (g_frameChain.load(std::memory_order_relaxed) < 0 && g_chainSamples < 300 && fn)       // learn where FFrame keeps its property chain
        {
            if (FField* pr = Rf::PropsOf(reinterpret_cast<UStruct*>(fn)))
            {
                g_chainSamples++;
                for (int k = 0; k < 14; k++)
                    if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(stack) + 0x30 + k * 8) == pr) g_chainVotes[k]++;
                if (g_chainSamples == 300)
                {
                    int best = 0;
                    for (int k = 1; k < 14; k++) if (g_chainVotes[k] > g_chainVotes[best]) best = k;
                    if (g_chainVotes[best] >= 280) { g_frameChain = 0x30 + best * 8; KLog(KLOG_INFO, "core", "script hook: FFrame property chain at +0x%x", (int)g_frameChain.load()); }
                    else KLog(KLOG_WARN, "core", "script hook: FFrame property chain not found (best %d/300): native hooks will see parameters but not change them", g_chainVotes[best]);
                }
            }
        }
        if (fn && LuaHost_HasHook(fn))
        {
            uint8_t* locals = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(stack) + kFrameLocals);
            t_busy = true; LuaHost_PreHook(ctx, fn, locals, result); t_busy = false;
            g_origPI(ctx, stack, result);
            t_busy = true; LuaHost_PostHook(ctx, fn, locals, result); t_busy = false;
            return;
        }
    }
    g_origPI(ctx, stack, result);
}

// ---------------------------------------------------------------------------------------------
// Native function hooks for calls the Blueprint VM makes directly (they never reach ProcessEvent). The VM calls a native
// function through UFunction::Func(Context, Stack, Result) with the arguments still sitting in the caller's BYTECODE; the
// function's exec thunk evaluates them. A hooked native function gets a small per-function thunk in place of Func: it
// evaluates the arguments itself with the engine's own opcode table (GNatives, found at run time), runs the Lua hooks on that
// buffer, and calls the original thunk with a synthetic frame (Code = null, Locals = our buffer: the same shape ProcessEvent
// uses), so changed arguments reach the function. Calls that come through ProcessEvent are left to the ProcessEvent detour.
// Not supported: functions with non-const reference/out parameters (their value lives in the caller's variable).
// ---------------------------------------------------------------------------------------------
using NativeFn = void (*)(UObject*, void*, void*);
struct NatSlot { UFunction* fn = nullptr; NativeFn orig = nullptr; std::vector<int> offs; size_t bufSize = 0; bool vmOk = false; };
static const int kNatSlots = 64;
static NatSlot g_nat[kNatSlots];
static std::atomic<int> g_natCount{0};
static uintptr_t* g_gnatives = nullptr;
static std::atomic<uint64_t> g_natVmCalls{0};

static void NatDispatch(int n, UObject* self, void* stack, void* result)
{
    NatSlot& s = g_nat[n];
    uint8_t* st = reinterpret_cast<uint8_t*>(stack);
    if (!s.vmOk || t_gameThread != 1 || t_busy || !st || !LuaHost_HasHook(s.fn)) { s.orig(self, stack, result); return; }
    uint8_t*& code = *reinterpret_cast<uint8_t**>(st + kCodeOff);
    if (*reinterpret_cast<UFunction**>(st + kFrameNode) == s.fn || !code) { s.orig(self, stack, result); return; }   // ProcessEvent path
    UObject* ctxObj = *reinterpret_cast<UObject**>(st + kFrameObject);
    uint8_t* const start = code;
    std::vector<uint8_t> buf(s.bufSize, 0);
    for (int off : s.offs)                                                       // FFrame::Step for each argument, in declaration order
    {
        uint8_t op = *code++;
        reinterpret_cast<NativeFn>(g_gnatives[op])(ctxObj, stack, buf.data() + off);
    }
    if (*code != 0x16)                                                           // EX_EndFunctionParms expected: otherwise let the function read its own arguments
    {
        code = start;
        s.orig(self, stack, result);
        return;
    }
    g_natVmCalls.fetch_add(1, std::memory_order_relaxed);
    uint8_t* const after = code + 1;
    t_busy = true; LuaHost_PreHook(self, s.fn, buf.data(), result); t_busy = false;
    int chain = g_frameChain.load(std::memory_order_relaxed);
    if (chain >= 0)
    {
        uint8_t fake[0xC0];
        memcpy(fake, st, sizeof(fake));
        *reinterpret_cast<UFunction**>(fake + kFrameNode) = s.fn;
        *reinterpret_cast<uint8_t**>(fake + kCodeOff) = nullptr;
        *reinterpret_cast<uint8_t**>(fake + kFrameLocals) = buf.data();
        *reinterpret_cast<FField**>(fake + chain) = Rf::PropsOf(reinterpret_cast<UStruct*>(s.fn));
        s.orig(self, fake, result);
        code = after;
    }
    else { code = start; s.orig(self, stack, result); }                          // read-only: the function evaluates the original bytecode again
    t_busy = true; LuaHost_PostHook(self, s.fn, buf.data(), result); t_busy = false;
}

template <int N> static void NatThunk(UObject* self, void* stack, void* result) { NatDispatch(N, self, stack, result); }
template <int... I> static std::array<NativeFn, sizeof...(I)> MakeThunks(std::integer_sequence<int, I...>) { return {{&NatThunk<I>...}}; }

// Called when a hook is registered. A Blueprint function needs nothing (the interpreter hook sees it). Returns false with a
// note when calls the Blueprint VM makes to this native function will not be seen.
bool KMod_EnsureNativeHook(UFunction* fn, std::string& note)
{
    if (!fn || !(Rf::FuncFlagsOf(fn) & 0x400)) return true;
    int cnt = g_natCount.load();
    for (int i = 0; i < cnt; i++) if (g_nat[i].fn == fn) return g_nat[i].vmOk;
    if (!g_gnatives) { note = "calls the Blueprint VM makes to this native function cannot be seen (opcode table not found); calls through ProcessEvent still are"; return false; }
    if (cnt >= kNatSlots) { note = "too many native function hooks"; return false; }
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(fn) + Rf::L.ufunc_exec);
    if (!*slot) { note = "the function has no native entry"; return false; }
    NatSlot& s = g_nat[cnt];
    s.fn = fn; s.orig = reinterpret_cast<NativeFn>(*slot); s.vmOk = true;
    size_t sz = Rf::ParamSizeOf(fn);
    for (FField* f = Rf::PropsOf(reinterpret_cast<UStruct*>(fn)); f; f = Rf::NextOf(f))
    {
        FProperty* p = reinterpret_cast<FProperty*>(f);
        uint64_t fl = Rf::PropFlagsOf(p);
        if (!(fl & 0x80)) continue;
        sz = std::max<size_t>(sz, Rf::OffsetOf(p) + (size_t)Rf::ElemSizeOf(p) * std::max(1, Rf::ArrayDimOf(p)));
        if (fl & 0x400) continue;                                                // return value: not in the bytecode
        if ((fl & 0x100) && !(fl & 0x2)) s.vmOk = false;                         // non-const out/reference parameter
        s.offs.push_back(Rf::OffsetOf(p));
    }
    s.bufSize = sz + 64;
    static const auto thunks = MakeThunks(std::make_integer_sequence<int, kNatSlots>());
    g_natCount.store(cnt + 1);
    if (!s.vmOk) { note = "the function has a reference/out parameter: calls from Blueprint cannot be hooked"; return false; }
    *slot = reinterpret_cast<uintptr_t>(thunks[cnt]);
    Log("native hook: %s (%d arguments%s)", Rf::PathName(reinterpret_cast<UObject*>(fn)).c_str(), (int)s.offs.size(), g_frameChain.load() >= 0 ? "" : ", read-only");
    return true;
}

void KMod_NativeStats(unsigned long long& vmCalls, int& slots, bool& haveTable, int& chain)
{
    vmCalls = g_natVmCalls.load(); slots = g_natCount.load(); haveTable = g_gnatives != nullptr; chain = g_frameChain.load();
}

static double NowSec()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void HookedProcessEvent(UObject* self, UFunction* fn, void* parms)
{
    g_peTotal.fetch_add(1, std::memory_order_relaxed);
    if (fn && (Rf::FuncFlagsOf(fn) & 0x400) == 0)
    {
        g_peScript.fetch_add(1, std::memory_order_relaxed);
        if (g_diag.load(std::memory_order_relaxed) && t_gameThread == 1 && g_diagPE.size() < 20000) g_diagPE.insert(fn);
    }
    if (t_gameThread < 0)
    {
        char name[32] = {};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        t_gameThread = (strcmp(name, "GameThread") == 0) ? 1 : 0;
        if (t_gameThread == 1) KBoot_SetGameThread((int)gettid());
        Log("ProcessEvent first seen on thread '%s' tid=%d -> gameThread=%d", name, (int)gettid(), t_gameThread);
    }

    // a Blueprint function is handled by the ProcessInternal detour (which also sees it when ProcessEvent runs it)
    const bool hooked = t_gameThread == 1 && !t_busy && LuaHost_HasHook(fn) &&
                        !(g_scriptHookOk.load(std::memory_order_relaxed) && (Rf::FuncFlagsOf(fn) & 0x400) == 0);
    if (hooked) { t_busy = true; LuaHost_PreHook(self, fn, parms); t_busy = false; }

    if (!g_frameLearned && t_gameThread == 1 && g_scriptHookOk.load(std::memory_order_relaxed) && fn && (Rf::FuncFlagsOf(fn) & 0x400) == 0 && parms)
    {
        t_expect.fn = fn; t_expect.self = self; t_expect.parms = parms;
        t_expect.hasHead = Rf::ParamSizeOf(fn) >= 8 && Resolver_SafeRead(reinterpret_cast<uintptr_t>(parms), &t_expect.head, 8) && t_expect.head > 0x10000;   // an object pointer or similar: not a common value
        t_expect.script = Rf::At<uintptr_t>(fn, Rf::L.ustruct_script);          // UStruct::Script (a TArray): its data pointer is the frame's Code at entry
    }
    g_origPE(self, fn, parms);
    t_expect = FrameExpect();

    if (t_busy) return;
    if (t_gameThread != 1) return;
    if (hooked) { t_busy = true; LuaHost_PostHook(self, fn, parms); t_busy = false; }

    static double lastTick = 0;
    const bool beat = (fn == g_hbFn);
    if (!beat && (++g_peCalls & 0x0F) != 0) return;
    double now = NowSec();
    static double lastFast = 0;
    if (LuaHost_HasFast() && now - lastFast >= 0.004)          // fast lane (FastLoop): every few ms, only when a mod asked for it
    {
        lastFast = now;
        t_busy = true;
        LuaHost_FastTick(now);
        t_busy = false;
    }
    if (now - lastTick < 0.05) return; // ~20 Hz
    lastTick = now;

    t_busy = true;
    OnGameTick();
    t_busy = false;
}

// ---------------------------------------------------------------------------------------------
// Pak mounting: kmod/paks/*.pak are mounted by calling the engine's own mount entry (HandleMountPakDelegate, bound to
// FCoreDelegates::MountPak). The engine picks the pak order from the path, which would put our paks BELOW the game's own,
// so the real mount worker (called with {path, order, ...} in x1) is hooked and the order is raised for paks that live in
// kmod/paks. All addresses are found at run time by resolver.cpp (no per-build offsets).
// ---------------------------------------------------------------------------------------------
using PakMountFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static PakMountFn g_origPakMount = nullptr;
static std::atomic<int> g_pakMounts{0};
static PakAddrs g_pak;
static bool g_pakTried = false, g_pakReady = false;

static bool WidePrefix(const char16_t* w, const std::string& prefix)
{
    for (size_t i = 0; i < prefix.size(); i++) if (w[i] != (char16_t)(unsigned char)prefix[i]) return false;
    return true;
}

static uint64_t HookedPakMount(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    const char16_t* path = a1 ? *reinterpret_cast<const char16_t* const*>(a1) : nullptr;
    if (path)
    {
        static const std::string prefix = KModRoot() + "/paks/";
        static const std::string alt = "/storage/emulated/0/Android/media/" + KPackageName() + "/kmod/paks/";
        int32_t* order = reinterpret_cast<int32_t*>(a1 + 8);
        std::string shown;
        for (int i = 0; i < 200 && path[i]; i++) shown += (path[i] < 128 ? (char)path[i] : '?');
        if (WidePrefix(path, prefix) || WidePrefix(path, alt))
        {
            int n = g_pakMounts.fetch_add(1);
            Log("pak mount: %s (order %d -> %d)", shown.c_str(), (int)*order, 100 + n);
            *order = 100 + n;      // later mounts win over earlier ones; all of them win over the game's paks
        }
        else Log("pak mount (game): %s order=%d", shown.c_str(), (int)*order);
    }
    return g_origPakMount(a0, a1, a2, a3, a4, a5, a6, a7);
}

static bool EnsurePak(uintptr_t base)
{
    if (g_pakTried) return g_pakReady;
    g_pakTried = true;
    std::string rep;
    if (!Resolver_ResolvePak(base, g_pak, rep)) { KLog(KLOG_WARN, "core", "pak mounting unavailable (unsupported game build?): %s", rep.c_str()); return false; }
    Log("pak layer: %s", rep.c_str());
    void* ptramp = nullptr;
    if (InstallInlineHook(reinterpret_cast<void*>(g_pak.worker), reinterpret_cast<void*>(&HookedPakMount), &ptramp)) g_origPakMount = reinterpret_cast<PakMountFn>(ptramp);
    else KLog(KLOG_WARN, "core", "pak mount hook failed: paks will be mounted with the engine's default (lowest) priority");
    g_pakReady = true;
    return true;
}

// Game thread. Mounts a pak file through the engine (the order is raised by HookedPakMount for kmod/paks).
bool KMod_MountPak(const std::string& path)
{
    uintptr_t base = InSDKUtils::GetImageBase();
    if (!base || !EnsurePak(base) || !g_pak.pakFile) return false;
    std::u16string w;
    for (unsigned char c : path) w.push_back(c);          // paths are ASCII in practice; UTF-8 bytes are widened one by one
    struct { const char16_t* data; int32_t num; int32_t max; } fs = {w.c_str(), (int32_t)w.size() + 1, (int32_t)w.size() + 1};
    using Fn = uint64_t (*)(void*, void*, int32_t);
    uint64_t r = reinterpret_cast<Fn>(g_pak.mountFn)(g_pak.pakFile, &fs, -1);
    Log("pak mount: %s -> %d", path.c_str(), (int)(r & 0xff));
    return (r & 0xff) != 0;
}

// kmod/paks/*.pak: mounted from here, at "engine ready", so the paks are in place before the game loads most of its content.
static std::vector<std::pair<std::string, std::string>> g_pakStatus;        // game thread only

static std::vector<std::string> ListPakFiles()
{
    std::vector<std::string> names;
    if (DIR* d = opendir((KModRoot() + "/paks").c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".pak") == 0) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

static std::string* PakStatusRef(const std::string& name)
{
    for (auto& p : g_pakStatus) if (p.first == name) return &p.second;
    g_pakStatus.emplace_back(name, "");
    return &g_pakStatus.back().second;
}

void KMod_MountPendingPaks()
{
    for (const std::string& name : ListPakFiles())
    {
        std::string* st = PakStatusRef(name);
        if (*st == "mounted" || *st == "failed") continue;                       // handled once per game run
        if (KBoot_SafeMode()) { *st = "skipped (safe mode)"; continue; }
        if (KBoot_IsQuarantined("pak", name)) { *st = "skipped (it crashed the game)"; KLog(KLOG_WARN, "core", "pak %s crashed the game last time: not mounted", name.c_str()); continue; }
        if (KBoot_IsQuarantined("off", name)) { *st = "switched off"; continue; }
        KBoot_SetActivity("pak", name.c_str(), "");                              // a crash while mounting is blamed on the pak
        bool ok = KMod_MountPak(KModRoot() + "/paks/" + name);
        KBoot_SetActivity(nullptr, nullptr, nullptr);
        *st = ok ? "mounted" : "failed";
        if (ok) Log("mounted pak %s", name.c_str()); else KLog(KLOG_WARN, "core", "could not mount pak %s", name.c_str());
    }
}

std::vector<std::pair<std::string, std::string>> KMod_PakStatus()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& name : ListPakFiles())
    {
        std::string st = *PakStatusRef(name);
        if (st.empty()) st = KBoot_IsQuarantined("off", name) ? "switched off" : "not mounted yet";
        out.emplace_back(name, st);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Layout detection: libkmodmd7.so (MobileDumper-7 run in-process, see core/md7) writes the engine's structure layout to
// <root>/boot/layout.json; here it is compared with the layout the SDK compiled into this core assumes.
// ---------------------------------------------------------------------------------------------
static std::atomic<int> g_layoutState{0};              // 0 idle, 1 running, 2 done
static std::atomic<bool> g_layoutGate{false};          // open = the Lua host may start (verification finished or not possible)
static std::atomic<bool> g_layoutMismatch{false};      // the engine's layout differs from the SDK compiled into this core
static std::string g_layoutReport = "layout verification has not run";
static std::mutex g_layoutMu;

static long long JsonNum(const std::string& js, const char* key, long long def = -9999)
{
    size_t k = js.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return def;
    size_t c = js.find(':', k);
    return c == std::string::npos ? def : atoll(js.c_str() + c + 1);
}

static std::string PackageName()
{
    char buf[256] = {};
    if (FILE* f = fopen("/proc/self/cmdline", "r")) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0; }
    return buf;
}

static std::string ReadWhole(const std::string& p)
{
    std::string out;
    if (FILE* f = fopen(p.c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); }
    return out;
}

// Compares a layout.json with the layout the SDK compiled into this core assumes.
static void EvaluateLayout(const std::string& js, const std::string& origin, double secs)
{
    struct Item { const char* key; long long compiled; };
    const Item items[] = {
        {"UObject.Flags", (long long)offsetof(UObject, Flags)}, {"UObject.Index", (long long)offsetof(UObject, Index)},
        {"UObject.Class", (long long)offsetof(UObject, Class)}, {"UObject.Name", (long long)offsetof(UObject, Name)},
        {"UObject.Outer", (long long)offsetof(UObject, Outer)},
        {"UField.Next", (long long)offsetof(UField, Next)},
        {"UStruct.SuperStruct", (long long)offsetof(UStruct, SuperStruct)}, {"UStruct.Children", (long long)offsetof(UStruct, Children)},
        {"UStruct.ChildProperties", (long long)offsetof(UStruct, ChildProperties)}, {"UStruct.Size", (long long)offsetof(UStruct, Size)},
        {"UFunction.FunctionFlags", (long long)offsetof(UFunction, FunctionFlags)}, {"UFunction.ExecFunction", (long long)offsetof(UFunction, ExecFunction)},
        {"FField.Next", (long long)offsetof(FField, Next)}, {"FField.Name", (long long)offsetof(FField, Name)},
        {"FField.Class", (long long)offsetof(FField, ClassPrivate)},
        {"Property.ArrayDim", (long long)offsetof(FProperty, ArrayDim)}, {"Property.ElementSize", (long long)offsetof(FProperty, ElementSize)},
        {"Property.PropertyFlags", (long long)offsetof(FProperty, PropertyFlags)}, {"Property.Offset_Internal", (long long)offsetof(FProperty, Offset)},
        {"Statics.PEIndex", (long long)Offsets::ProcessEventIdx},
    };
    int same = 0, diff = 0, missing = 0;
    std::string diffs;
    bool peMoved = false;
    for (auto& it : items)
    {
        long long det = JsonNum(js, it.key);
        if (det == -9999) { missing++; continue; }
        if (det == it.compiled) same++;
        else
        {
            diff++;
            if (strcmp(it.key, "Statics.PEIndex") == 0) peMoved = true;     // baked into the SDK's ProcessEvent: cannot be adopted
            char b[120]; snprintf(b, sizeof(b), " %s: engine 0x%llx, SDK 0x%llx;", it.key, det, it.compiled); diffs += b;
        }
    }
    Rf::Load(js);                                  // from here on all reflection reads use the engine's real layout
    char head[220];
    snprintf(head, sizeof(head), "engine layout (%s%s): %d value(s) match the SDK in this core, %d differ, %d not reported.",
             origin.c_str(), secs > 0 ? (", detected in " + std::to_string((int)secs) + " s").c_str() : "", same, diff, missing);
    std::string rep = std::string(head) + diffs;
    bool block = peMoved || same == 0;
    if (block) { g_layoutMismatch = true; rep += " -> mods are NOT loaded (the layout cannot be adopted)."; }
    else if (diff > 0) rep += " -> adopted: the engine's layout is used instead of the SDK's.";
    { std::lock_guard<std::mutex> g(g_layoutMu); g_layoutReport = rep; }
    KLog(block ? KLOG_ERROR : (diff > 0 ? KLOG_WARN : KLOG_INFO), "layout", "%s", rep.c_str());
}

// The detector: libkmodmd7.so, bundled with the app (release) or at kmod/dev/ (development).
static void* OpenDetector(const std::string& pkg)
{
    if (void* h = dlopen("libkmodmd7.so", RTLD_NOW | RTLD_LOCAL)) return h;
    std::string src = KModRoot() + "/dev/libkmodmd7.so";
    std::string dstDir = "/data/data/" + pkg + "/files/kmod";
    std::string dst = dstDir + "/libkmodmd7.so";
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return nullptr;
    mkdir(dstDir.c_str(), 0777);
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return nullptr; }
    char b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof(b), in)) > 0) fwrite(b, 1, n, out);
    fclose(in); fclose(out);
    return dlopen(dst.c_str(), RTLD_NOW | RTLD_LOCAL);
}

// Verification: a cached layout.json for this engine build is compared at once; without one the detector runs (about 15 s)
// and its result is cached. The Lua host waits for this (g_layoutGate) so no mod runs on an unverified layout.
static void LayoutMain(bool force)
{
    struct Gate { ~Gate() { g_layoutState = 2; g_layoutGate = true; } } gate;
    std::string key = Resolver_BuildKey();
    if (key.empty()) { std::lock_guard<std::mutex> g(g_layoutMu); g_layoutReport = "layout verification skipped (engine library not found)"; return; }
    mkdir((KModRoot() + "/boot").c_str(), 0777);
    std::string cache = KModRoot() + "/boot/layout-" + key + ".json";
    if (!force)
    {
        std::string js = ReadWhole(cache);
        if (js.find("\"ok\"") != std::string::npos) { EvaluateLayout(js, "cached", 0); return; }
    }
    std::string pkg = PackageName();
    void* h = OpenDetector(pkg);
    using Fn = int (*)(const char*, const char*, const char*);
    Fn run = h ? reinterpret_cast<Fn>(dlsym(h, "kmod_md7_run")) : nullptr;
    if (!run)
    {
        std::lock_guard<std::mutex> g(g_layoutMu);
        g_layoutReport = "engine layout not verified: no cached result for build " + key + " and libkmodmd7.so is not available";
        KLog(KLOG_WARN, "layout", "%s", g_layoutReport.c_str());
        return;
    }
    remove(cache.c_str());
    double t0 = NowSec();
    int rc = run(pkg.c_str(), cache.c_str(), (KModRoot() + "/logs/md7.log").c_str());
    double secs = NowSec() - t0;
    if (rc != 0)
    {
        std::lock_guard<std::mutex> g(g_layoutMu);
        g_layoutReport = "engine layout detection failed (see logs/md7.log); continuing unverified";
        KLog(KLOG_WARN, "layout", "%s", g_layoutReport.c_str());
        return;
    }
    EvaluateLayout(ReadWhole(cache), "detected", secs);
}

// Starts the verification in the background. force = ignore the cache and detect again.
bool KMod_DetectLayout(bool force)
{
    int expected = 0;
    if (force) { g_layoutState = 0; g_layoutMismatch = false; }
    if (!g_layoutState.compare_exchange_strong(expected, 1) && !force) return false;
    if (force) g_layoutState = 1;
    std::thread(LayoutMain, force).detach();
    return true;
}

std::string KMod_LayoutReport()
{
    std::lock_guard<std::mutex> g(g_layoutMu);
    return std::string(g_layoutState == 1 ? "running... " : "") + g_layoutReport;
}

bool KMod_LayoutMismatch() { return g_layoutMismatch.load(); }

// Diagnostics: kmod.script_diag(true) starts collecting, kmod.script_diag(false) stops and returns the paths of the script
// functions that ran through ProcessInternal but never through ProcessEvent: calls the Blueprint VM made directly.
std::vector<std::string> KMod_ScriptDiag(bool start)
{
    std::vector<std::string> out;
    if (start) { g_diagPI.clear(); g_diagPE.clear(); g_diag = true; return out; }
    g_diag = false;
    for (UFunction* f : g_diagPI)
        if (!g_diagPE.count(f) && out.size() < 40) out.push_back(Rf::PathName(reinterpret_cast<UObject*>(f)));
    out.push_back("-- seen via ProcessInternal: " + std::to_string(g_diagPI.size()) + ", via ProcessEvent: " + std::to_string(g_diagPE.size()));
    g_diagPI.clear(); g_diagPE.clear();
    return out;
}

// Counters for the script-function hook: calls that went through ProcessInternal, and through ProcessEvent.
void KMod_ScriptStats(unsigned long long& pi, unsigned long long& pe, bool& installed)
{
    pi = g_piTotal.load(); pe = g_peScript.load(); installed = g_scriptHookOk.load();   // pe here = ProcessEvent calls of script functions
}

// Finds ProcessInternal as the ExecFunction that every script (non-native) UFunction shares, and hooks it.
static void EnsureScriptHook(uintptr_t base)
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    UClass* fcls = Rf::FunctionClass();
    TUObjectArray* arr = InSDKUtils::GetGObjects();
    if (!fcls || !arr) return;
    std::map<uintptr_t, int> votes;
    int seen = 0;
    for (int32_t i = 0; i < arr->Num() && seen < 600; i++)
    {
        FUObjectItem* it = ObjRef::ItemAt(i);
        UObject* o = it ? it->Object : nullptr;
        if (!o || !ObjRef::ItemUsable(it) || Rf::ClassOf(o) != fcls) continue;
        if (Rf::FuncFlagsOf(reinterpret_cast<UFunction*>(o)) & 0x400) continue;            // native
        uintptr_t f = Rf::At<uintptr_t>(o, Rf::L.ufunc_exec);
        if (f) { votes[f]++; seen++; }
    }
    uintptr_t best = 0; int bestN = 0;
    for (auto& kv : votes) if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
    if (!best || seen < 50 || bestN * 10 < seen * 9)
    {
        KLog(KLOG_WARN, "core", "script hook: no common ProcessInternal found (%d of %d); Blueprint-internal calls stay invisible", bestN, seen);
        return;
    }
    // ProcessInternal (the shared ExecFunction) ends by jumping to ProcessLocalScriptFunction, the bytecode interpreter (a loop
    // dispatching through GNatives[opcode]); the Blueprint VM calls that interpreter directly for Blueprint-to-Blueprint calls.
    // It is the one place every script call passes: find the branch target whose first instructions index a table by opcode.
    uintptr_t plsf = 0;
    {
        const uint32_t* code = reinterpret_cast<const uint32_t*>(best);
        for (int i = 0; i < 0x180 && !plsf; i++)
        {
            uint32_t ins = code[i];
            if ((ins & 0xFC000000) != 0x14000000) continue;                                  // B (unconditional)
            int64_t imm = ins & 0x3ffffff; if (imm & 0x2000000) imm -= 0x4000000;
            uintptr_t target = best + (uintptr_t)i * 4 + imm * 4;
            if (target <= best || target > best + 0x800) continue;
            const uint32_t* t = reinterpret_cast<const uint32_t*>(target);
            bool tableLoad = false, call = false;
            for (int k = 0; k < 0x24; k++)
            {
                if ((t[k] & 0xFFE0FC00) == 0xF8607800) tableLoad = true;                      // ldr Xt, [Xn, Xm, lsl #3]
                if ((t[k] & 0xFFFFFC1F) == 0xD63F0000) call = true;                           // blr Xn
            }
            if (tableLoad && call) plsf = target;
        }
    }
    uintptr_t hookAt = plsf ? plsf : 0;
    if (plsf)
    {
        // GNatives: the interpreter loads its address with adrp+add and then indexes it by opcode (ldr Xt,[Xn,Xm,lsl #3]).
        const uint32_t* t = reinterpret_cast<const uint32_t*>(plsf);
        for (int i = 0; i < 0x30 && !g_gnatives; i++)
        {
            uint32_t a = t[i];
            if ((a & 0x9F000000) != 0x90000000) continue;                                 // adrp
            int rd = a & 31;
            int64_t imm = (int64_t)((((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3));
            if (imm & 0x100000) imm -= 0x200000;
            uintptr_t page = ((plsf + (uintptr_t)i * 4) & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
            for (int j = i + 1; j < i + 4 && j < 0x30; j++)
            {
                uint32_t b = t[j];
                if ((b & 0xFFC00000) != 0x91000000 || (int)(b & 31) != rd || (int)((b >> 5) & 31) != rd) continue;   // add Xd, Xd, #imm
                uintptr_t tbl = page + ((b >> 10) & 0xFFF);
                bool indexed = false;
                for (int k = j + 1; k < 0x30; k++) if ((t[k] & 0xFFE0FC00) == 0xF8607800 && (int)((t[k] >> 5) & 31) == rd) indexed = true;
                if (!indexed) continue;
                const uintptr_t* e = reinterpret_cast<const uintptr_t*>(tbl);
                int good = 0;
                for (int q = 0; q < 256; q++) if (e[q] > base && e[q] < base + 0x20000000 && (e[q] & 3) == 0) good++;
                if (good >= 250) { g_gnatives = const_cast<uintptr_t*>(e); Log("GNatives at 0x%lx (%d/256 entries valid)", (unsigned long)(tbl - base), good); }
            }
        }
        if (!g_gnatives) KLog(KLOG_WARN, "core", "script hook: opcode table (GNatives) not found; native functions called from Blueprint stay invisible");
    }
    if (!hookAt)
    {
        KLog(KLOG_WARN, "core", "script hook: ProcessLocalScriptFunction not found behind ProcessInternal 0x%lx; Blueprint-internal calls stay invisible", (unsigned long)(best - base));
        return;
    }
    void* tramp = nullptr;
    if (!InstallInlineHook(reinterpret_cast<void*>(hookAt), reinterpret_cast<void*>(&HookedProcessInternal), &tramp))
    {
        KLog(KLOG_WARN, "core", "script hook: cannot hook the script interpreter at 0x%lx", (unsigned long)(hookAt - base));
        return;
    }
    g_origPI = reinterpret_cast<ProcessInternalFn>(tramp);
    g_scriptHookOk = true;
    Log("script hook installed: interpreter at 0x%lx (ProcessInternal 0x%lx, %d/%d script functions share it)", (unsigned long)(hookAt - base), (unsigned long)(best - base), bestN, seen);
}

static double g_readySince = 0;
static double g_lastHeartbeatArm = 0;

// Game thread. Arms (or re-arms: the timer is replaced, not duplicated) the heartbeat; also follows world changes.
static void ArmHeartbeat(double now)
{
    if (now - g_lastHeartbeatArm < 2.0) return;
    g_lastHeartbeatArm = now;
    UObject* world = CurrentWorld();
    if (!world) return;
    Rf::Args gp = Rf::Prepare("GameplayStatics", "GetPlayerController");
    gp.Set("WorldContextObject", world).Set("PlayerIndex", (int32_t)0);
    gp.Call(Rf::CdoFor("GameplayStatics"));
    UObject* pc = gp.Get<UObject*>("ReturnValue");
    if (!pc) return;
    if (!g_hbFn) g_hbFn = Rf::FindFunc(reinterpret_cast<UStruct*>(Rf::ClassOf(pc)), "ForceNetUpdate");
    if (!g_hbFn) return;
    Rf::Args t = Rf::Prepare("KismetSystemLibrary", "K2_SetTimer");
    static const char16_t kName[] = u"ForceNetUpdate";
    t.Set("Object", pc).Set("FunctionName", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kName)), 15, 15));
    t.Set("Time", 0.016f).Set("bLooping", true).Set("bMaxOncePerFrame", false).Set("InitialStartDelay", 0.f).Set("InitialStartDelayVariance", 0.f);
    t.Call(Rf::CdoFor("KismetSystemLibrary"));
}

// The game engine object (cached), its viewport and world: read by property name through the runtime reflection layer.
static UObject* CurrentWorld()
{
    static UObject* engine = nullptr;
    static int32_t engineIdx = -1;
    if (engine)                                                   // still the same live object?
    {
        FUObjectItem* it = ObjRef::ItemAt(engineIdx);
        if (!it || it->Object != engine || !ObjRef::ItemUsable(it)) engine = nullptr;
    }
    if (!engine)
    {
        engine = Rf::FindFirst("GameEngine");
        if (!engine) return nullptr;
        engineIdx = Rf::IndexOf(engine);
    }
    UObject* vp = Rf::ObjProp(engine, "GameViewport");
    return vp ? Rf::ObjProp(vp, "World") : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Canaries: the layouts that come from the SDK compiled into this core (object table geometry, FString, FText, TArray) are checked
// against the running game once the engine is up. A wrong object table or FString layout would make every mod read garbage, so those
// two block the mods (like a reflection layout mismatch); FText and TArray only warn (the features that use them report errors).
// ---------------------------------------------------------------------------------------------
static std::string g_canaryReport;

static void RunCanaries()
{
    std::string rep;
    bool block = false;

    // 1. object table: every object's own Index must equal its slot (validates Objects/chunk layout, FUObjectItem stride and UObject.Index)
    {
        TUObjectArray* arr = InSDKUtils::GetGObjects();
        int total = 0, good = 0;
        int32_t n = arr ? arr->Num() : 0;
        int32_t step = n > 4000 ? n / 4000 : 1;
        for (int32_t i = 0; i < n; i += step)
        {
            FUObjectItem* it = ObjRef::ItemAt(i);
            if (!it || !it->Object) continue;
            total++;
            if (Rf::IndexOf(it->Object) == i) good++;
        }
        bool ok = total >= 200 && good * 100 >= total * 99;
        char b[160]; snprintf(b, sizeof(b), "object table %s (%d/%d objects at their own index)", ok ? "ok" : "WRONG", good, total);
        rep += b; if (!ok) block = true;
    }

    // 2. FString: concatenate through the engine and read the result back
    {
        static const char16_t kAb[] = u"ab", kCd[] = u"cd";
        Rf::Args a = Rf::Prepare("KismetStringLibrary", "Concat_StrStr");
        a.Set("A", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kAb)), 3, 3));
        a.Set("B", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kCd)), 3, 3));
        bool ok = false;
        if (a.ok() && a.Call(Rf::CdoFor("KismetStringLibrary"))) ok = a.Get<FString>("ReturnValue").ToString() == "abcd";
        rep += std::string("; FString ") + (ok ? "ok" : "WRONG");
        if (!ok) block = true;
    }

    // 3. FText: string -> text -> string through the engine
    {
        static const char16_t kT[] = u"kmod";
        Rf::Args a = Rf::Prepare("KismetTextLibrary", "Conv_StringToText");
        a.Set("InString", FString(const_cast<TCHAR*>(reinterpret_cast<const TCHAR*>(kT)), 5, 5));
        bool ok = false;
        if (a.ok() && a.Call(Rf::CdoFor("KismetTextLibrary")))
        {
            FText t = a.Get<FText>("ReturnValue");
            Rf::Args b = Rf::Prepare("KismetTextLibrary", "Conv_TextToString");
            b.Set("InText", t);
            if (b.ok() && b.Call(Rf::CdoFor("KismetTextLibrary"))) ok = b.Get<FString>("ReturnValue").ToString() == "kmod";
        }
        rep += std::string("; FText ") + (ok ? "ok" : "WRONG (FText properties may misbehave)");
    }

    // 4. TArray: GameInstance.LocalPlayers is a short array of valid objects
    {
        bool ok = false;
        if (UObject* gi = Rf::FindFirst("GameInstance"))
            if (FProperty* p = Rf::FindProp(reinterpret_cast<UStruct*>(Rf::ClassOf(gi)), "LocalPlayers"))
            {
                struct Raw { void* data; int32_t num, max; };
                const Raw* r = &Rf::At<Raw>(gi, Rf::OffsetOf(p));
                if (r->data && r->num >= 1 && r->num <= 16 && r->max >= r->num)
                {
                    UObject* first = *static_cast<UObject**>(r->data);
                    ok = first && Rf::ClassOf(first) && Rf::FindClass("LocalPlayer") && Rf::IsA(first, Rf::FindClass("LocalPlayer"));
                }
            }
        rep += std::string("; TArray ") + (ok ? "ok" : "WRONG (array properties may misbehave)");
    }

    g_canaryReport = rep;
    KLog(block ? KLOG_ERROR : KLOG_INFO, "layout", "canaries: %s%s", rep.c_str(), block ? " -> mods are NOT loaded" : "");
    if (block)
    {
        g_layoutMismatch = true;
        std::lock_guard<std::mutex> g(g_layoutMu);
        g_layoutReport += " | canaries: " + rep + " -> mods are NOT loaded (the SDK layouts compiled into this core do not match the game).";
    }
}

// Runs on the game thread (throttled ~20 Hz) from the ProcessEvent hook.
static void OnGameTick()
{
    // Nothing that reads reflection data may run before the engine's layout was checked (and, if it moved, adopted):
    // the first run on a new game build detects it first (about 15 s).
    static double gateSince = 0;
    if (!g_layoutGate.load())
    {
        double t = NowSec();
        if (gateSince == 0) gateSince = t;
        if (t - gateSince < 90.0) return;
        g_layoutGate = true;
    }
    if (!CurrentWorld()) return;
    EnsureScriptHook(InSDKUtils::GetImageBase());          // once, with the engine layout already adopted
    { static bool canaries = false; if (!canaries) { canaries = true; RunCanaries(); } }       // before any mod runs

    double now = NowSec();
    if (g_readySince == 0) { g_readySince = now; Log("engine ready"); KMod_MountPendingPaks(); }
    ArmHeartbeat(now);
    if (now - g_readySince < 5.0) return; // let the game settle
    static bool stableMarked = false;
    if (!stableMarked && now - g_readySince > 60.0) { stableMarked = true; KBoot_MarkStable(); }

    LuaHost_Init();          // no-op after the first call
    LuaHost_Tick(now);
}

// ---------------------------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------------------------
static void InitThread()
{
    Log("core init thread started");
    uintptr_t base = 0;
    std::string report;
    bool ok = false;
    for (int i = 0; i < 600 && !ok; i++)      // up to 5 minutes
    {
        base = InSDKUtils::GetImageBase();
        if (base && Resolver_ResolveCore(base, report))
        {
            TUObjectArray* objs = InSDKUtils::GetGObjects();
            ok = objs && objs->Num() > 50000;
        }
        if (!ok) usleep(500 * 1000);
    }
    if (!base) { KLog(KLOG_ERROR, "core", "libUnreal.so not found"); return; }
    if (!ok)
    {
        KLog(KLOG_ERROR, "core", "this game build is not supported (could not locate the engine data): %s. kmod stays inactive.", report.c_str());
        return;
    }
    Log("resolved: %s", report.c_str());
    sleep(3);

    void* target = reinterpret_cast<void*>(base + Offsets::ProcessEvent);
    Log("image base=0x%lx ProcessEvent=%p objects=%d", (unsigned long)base, target, InSDKUtils::GetGObjects()->Num());
    void* tramp = nullptr;
    if (!InstallInlineHook(target, reinterpret_cast<void*>(&HookedProcessEvent), &tramp)) { KLog(KLOG_ERROR, "core", "hook install FAILED"); return; }
    g_origPE = reinterpret_cast<ProcessEventFn>(tramp);
    Log("ProcessEvent hook installed, trampoline=%p", tramp);
    KLogInstallCrashHandlers();      // the engine installs its own handlers during start-up; (re)install ours on top of them
    KMod_DetectLayout(false);        // verify the engine layout against the SDK in this core (cached per game build)
}

// The folders a user puts files into must exist as soon as the game has started once (not only when a feature first needs them):
// kmod/mods (mod folders and .kmod packages), kmod/paks (pak mods), kmod/data (mod data).
static void EnsureUserFolders()
{
    std::string root = KModRoot();
    std::string path;
    for (size_t i = 1; i <= root.size(); i++)
        if (i == root.size() || root[i] == '/') mkdir(root.substr(0, i).c_str(), 0777);
    for (const char* sub : {"mods", "paks", "data"}) mkdir((root + "/" + sub).c_str(), 0777);
}

__attribute__((constructor)) static void CoreMain()
{
    KLogInit();
    EnsureUserFolders();
    KMod_ApplyStagedPaks();      // pak updates staged by the last run, before the game mounts its paks
    KBoot_Init();
    NetRedirect_Start();          // before anything else: the game reads its server URLs early
    Log("libkmodcore loaded");
    std::thread(InitThread).detach();
}
