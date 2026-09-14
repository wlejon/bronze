// The natives every test in this directory registers — the C side of one
// small host, written once so the JIT suite (native_test.cpp), the manifest
// writer (manifest_tool.cpp) and the shared-load harness (harness.cpp) bind
// the SAME functions under the SAME paths with the SAME signatures. The
// harness's whole claim is that a module compiled against the manifest the
// tool printed binds to what the harness registers; two copies of this list
// would let the two drift and the test would be testing the drift.
//
// Everything is `static` so that each translation unit gets its own copy of
// the functions AND the counters: the harness counts destructor runs in its
// own process, the doctest binary in its own.
//
// Namespace `nt`:
//   nt.math.add(f64, f64) -> f64          nt.math.bits(i32, i32) -> i32
//   nt.math.flip(bool) -> bool            nt.math.add3(f64,f64,f64) -> f64
//   nt.str.greet(str) -> str              nt.str.len(str) -> f64
//   nt.dyn.identity(dynamic) -> dynamic
//   nt.arr.sum{F32,F64,I32,U8,U16,U32,I8,I16}(<kind>[]) -> f64
//   nt.arr.fill(f64[], f64) -> void       (writes through the pointer)
//   nt.time.scale                          namespace property (getter+setter)
//   class nt.Agent(f64 hp): hit(f64)->f64, id()->f64, hp get/set,
//                           target()->nt.Target, label()->str; destructor
//   class nt.Target(f64):   value()->f64
//   nt.peek(nt.Agent) -> f64               a function taking a class handle
//   nt.absent.ping() -> f64                registered ONLY when the caller
//                                          asks: the native a module can be
//                                          compiled against and not find

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "embed/embed.h"

namespace nt_natives {

// ---- scalars ---------------------------------------------------------------

static double add(double a, double b) { return a + b; }
static double add3(double a, double b, double c) { return a + b + c; }
static int32_t bits(int32_t a, int32_t b) { return a & b; }
static bool flip(bool b) { return !b; }

// ---- strings ---------------------------------------------------------------

static char g_greetBuffer[256];
static const char* greet(const char* name) {
    std::snprintf(g_greetBuffer, sizeof g_greetBuffer, "hi, %s", name);
    return g_greetBuffer;
}
static double strLen(const char* s) { return static_cast<double>(std::strlen(s)); }

// ---- dynamic ---------------------------------------------------------------

static uint64_t identity(uint64_t v) { return v; }

// ---- typed arrays ----------------------------------------------------------

template <typename T>
static double sumOf(const T* data, uint32_t n) {
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) total += static_cast<double>(data[i]);
    return total;
}
static double sumF32(const float* d, uint32_t n) { return sumOf(d, n); }
static double sumF64(const double* d, uint32_t n) { return sumOf(d, n); }
static double sumI32(const int32_t* d, uint32_t n) { return sumOf(d, n); }
static double sumU8(const uint8_t* d, uint32_t n) { return sumOf(d, n); }
static double sumU16(const uint16_t* d, uint32_t n) { return sumOf(d, n); }
static double sumU32(const uint32_t* d, uint32_t n) { return sumOf(d, n); }
static double sumI8(const int8_t* d, uint32_t n) { return sumOf(d, n); }
static double sumI16(const int16_t* d, uint32_t n) { return sumOf(d, n); }
static void fillF64(double* d, uint32_t n, double v) {
    for (uint32_t i = 0; i < n; ++i) d[i] = v * static_cast<double>(i);
}

// ---- a namespace property --------------------------------------------------

static double g_timeScale = 1.0;
static double timeScaleGet() { return g_timeScale; }
static void timeScaleSet(double v) { g_timeScale = v; }

// ---- classes ---------------------------------------------------------------

struct Target {
    double value;
};
// nt.Target registers NO destructor, because a Target handle is also what
// nt.Agent.target() answers — a view into the agent — and one class owns one
// destructor. Standalone targets therefore come from a static pool rather
// than the free store, so nothing here leaks or double-frees.
static Target g_targetPool[64];
static int g_targetsMade = 0;
static void* targetNew(double v) {
    Target* t = &g_targetPool[g_targetsMade++ % 64];
    t->value = v;
    return t;
}
static double targetValue(void* self) { return static_cast<Target*>(self)->value; }

struct Agent {
    double hp;
    double id;
    Target target;
    std::string label;
};
static int g_agentsMade = 0;
static int g_agentsFreed = 0;
static void* agentNew(double hp) {
    ++g_agentsMade;
    return new Agent{hp, static_cast<double>(g_agentsMade), Target{hp * 2}, "agent#" + std::to_string(g_agentsMade)};
}
static void agentFree(void* p) {
    ++g_agentsFreed;
    delete static_cast<Agent*>(p);
}
static double agentHit(void* self, double dmg) {
    auto* a = static_cast<Agent*>(self);
    a->hp -= dmg;
    return a->hp;
}
static double agentId(void* self) { return static_cast<Agent*>(self)->id; }
static double agentHpGet(void* self) { return static_cast<Agent*>(self)->hp; }
static void agentHpSet(void* self, double hp) { static_cast<Agent*>(self)->hp = hp; }
// A pointer the runtime wraps as a nt.Target handle. The Agent owns the
// Target, so the class registered for it carries NO destructor: a handle
// to it is a view, and freeing through it would free the agent's member.
static void* agentTarget(void* self) { return &static_cast<Agent*>(self)->target; }
static const char* agentLabel(void* self) { return static_cast<Agent*>(self)->label.c_str(); }

static double peek(void* agent) { return static_cast<Agent*>(agent)->hp; }

static double absentPing() { return 99; }

// ---- registration ----------------------------------------------------------

// Registers every native above on the calling thread. `withAbsent` adds
// nt.absent.ping, the one the "missing native" module is compiled against.
// Returns false with `err` set on the first refusal.
inline bool registerAll(std::string& err, bool withAbsent = false) {
    namespace embed = bronze::embed;
    using embed::NativeKind;
    using embed::NativeSignature;

    auto fn = [&](const char* path, void* f, const char* ret,
                  std::initializer_list<const char*> params) -> bool {
        NativeSignature sig;
        sig.returnType = ret;
        for (const char* p : params) sig.paramTypes.emplace_back(p);
        return embed::registerNative(path, f, sig, &err);
    };
    auto member = [&](NativeKind kind, const char* cls, const char* path, void* f, const char* ret,
                      std::initializer_list<const char*> params) -> bool {
        NativeSignature sig;
        sig.kind = kind;
        sig.className = cls;
        sig.returnType = ret;
        for (const char* p : params) sig.paramTypes.emplace_back(p);
        return embed::registerNative(path, f, sig, &err);
    };
    auto ctor = [&](const char* cls, void* f, bronze::embed::HandleDestructor dtor,
                    std::initializer_list<const char*> params) -> bool {
        NativeSignature sig;
        sig.kind = NativeKind::Constructor;
        sig.className = cls;
        sig.returnType = cls;
        sig.destructor = dtor;
        for (const char* p : params) sig.paramTypes.emplace_back(p);
        return embed::registerNative(cls, f, sig, &err);
    };

    return fn("nt.math.add", reinterpret_cast<void*>(&add), "f64", {"f64", "f64"}) &&
           fn("nt.math.add3", reinterpret_cast<void*>(&add3), "f64", {"f64", "f64", "f64"}) &&
           fn("nt.math.bits", reinterpret_cast<void*>(&bits), "i32", {"i32", "i32"}) &&
           fn("nt.math.flip", reinterpret_cast<void*>(&flip), "bool", {"bool"}) &&
           fn("nt.str.greet", reinterpret_cast<void*>(&greet), "str", {"str"}) &&
           fn("nt.str.len", reinterpret_cast<void*>(&strLen), "f64", {"str"}) &&
           fn("nt.dyn.identity", reinterpret_cast<void*>(&identity), "dynamic", {"dynamic"}) &&
           fn("nt.arr.sumF32", reinterpret_cast<void*>(&sumF32), "f64", {"f32[]"}) &&
           fn("nt.arr.sumF64", reinterpret_cast<void*>(&sumF64), "f64", {"f64[]"}) &&
           fn("nt.arr.sumI32", reinterpret_cast<void*>(&sumI32), "f64", {"i32[]"}) &&
           fn("nt.arr.sumU8", reinterpret_cast<void*>(&sumU8), "f64", {"u8[]"}) &&
           fn("nt.arr.sumU16", reinterpret_cast<void*>(&sumU16), "f64", {"u16[]"}) &&
           fn("nt.arr.sumU32", reinterpret_cast<void*>(&sumU32), "f64", {"u32[]"}) &&
           fn("nt.arr.sumI8", reinterpret_cast<void*>(&sumI8), "f64", {"i8[]"}) &&
           fn("nt.arr.sumI16", reinterpret_cast<void*>(&sumI16), "f64", {"i16[]"}) &&
           fn("nt.arr.fill", reinterpret_cast<void*>(&fillF64), "void", {"f64[]", "f64"}) &&
           member(NativeKind::Getter, "", "nt.time.scale", reinterpret_cast<void*>(&timeScaleGet),
                  "f64", {}) &&
           member(NativeKind::Setter, "", "nt.time.scale", reinterpret_cast<void*>(&timeScaleSet),
                  "void", {"f64"}) &&
           ctor("nt.Target", reinterpret_cast<void*>(&targetNew), nullptr, {"f64"}) &&
           member(NativeKind::Method, "nt.Target", "nt.Target.value",
                  reinterpret_cast<void*>(&targetValue), "f64", {}) &&
           ctor("nt.Agent", reinterpret_cast<void*>(&agentNew), &agentFree, {"f64"}) &&
           member(NativeKind::Method, "nt.Agent", "nt.Agent.hit", reinterpret_cast<void*>(&agentHit),
                  "f64", {"f64"}) &&
           member(NativeKind::Method, "nt.Agent", "nt.Agent.id", reinterpret_cast<void*>(&agentId),
                  "f64", {}) &&
           member(NativeKind::Getter, "nt.Agent", "nt.Agent.hp", reinterpret_cast<void*>(&agentHpGet),
                  "f64", {}) &&
           member(NativeKind::Setter, "nt.Agent", "nt.Agent.hp", reinterpret_cast<void*>(&agentHpSet),
                  "void", {"f64"}) &&
           member(NativeKind::Method, "nt.Agent", "nt.Agent.target",
                  reinterpret_cast<void*>(&agentTarget), "nt.Target", {}) &&
           member(NativeKind::Method, "nt.Agent", "nt.Agent.label",
                  reinterpret_cast<void*>(&agentLabel), "str", {}) &&
           fn("nt.peek", reinterpret_cast<void*>(&peek), "f64", {"nt.Agent"}) &&
           (!withAbsent || fn("nt.absent.ping", reinterpret_cast<void*>(&absentPing), "f64", {}));
}

}  // namespace nt_natives
