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
//   nt.buf.rangeF32(i32) -> f32[]         copy mode: 0..n-1 as floats
//   nt.buf.squaresI32(i32) -> i32[]       copy mode: i*i
//   nt.buf.ownedU8(i32) -> u8[]           TRANSFER mode: a malloc'd block
//                                          the runtime views in place; its
//                                          release is counted (g_bufReleased)
//   nt.buf.empty() -> f64[]               the empty array (null data)
//   nt.buf.throwAfterFill() -> u16[]      fills a transfer descriptor, then
//                                          throws: the release must still run
//   nt.time.scale                          namespace property (getter+setter)
//   class nt.Agent(f64 hp): hit(f64)->f64, id()->f64, hp get/set,
//                           target()->nt.Target, label()->str,
//                           stats()->f64[] (copy: [hp, id]); destructor
//   class nt.Target(f64):   value()->f64
//   nt.peek(nt.Agent) -> f64               a function taking a class handle
//   nt.absent.ping() -> f64                registered ONLY when the caller
//                                          asks: the native a module can be
//                                          compiled against and not find

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// ---- typed-array returns ---------------------------------------------------
//
// Copy mode (`release` left null): the descriptor names storage that is the
// native's own and valid for this call only; the runtime copies. Both of
// these hand out a static scratch, which is exactly the storage a copy is
// FOR — the next call overwrites it, and the program's array does not care.

static float g_rangeScratch[64];
static void rangeF32(int32_t n, bronze_native_buffer* out) {
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    for (int32_t i = 0; i < n; ++i) g_rangeScratch[i] = static_cast<float>(i);
    out->data = g_rangeScratch;
    out->length = static_cast<uint32_t>(n);
}

static int32_t g_squaresScratch[64];
static void squaresI32(int32_t n, bronze_native_buffer* out) {
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    for (int32_t i = 0; i < n; ++i) g_squaresScratch[i] = i * i;
    out->data = g_squaresScratch;
    out->length = static_cast<uint32_t>(n);
}

// Transfer mode: a fresh block the runtime views in place, released — freed,
// and counted — when the program's buffer is collected. `ctx` is the block.
static int g_bufMade = 0;
static int g_bufReleased = 0;
static void releaseOwned(void* ctx) {
    ++g_bufReleased;
    std::free(ctx);
}
static void ownedU8(int32_t n, bronze_native_buffer* out) {
    if (n < 0) n = 0;
    auto* block = static_cast<uint8_t*>(std::malloc(n > 0 ? static_cast<size_t>(n) : 1));
    for (int32_t i = 0; i < n; ++i) block[i] = static_cast<uint8_t>(i * 3);
    ++g_bufMade;
    out->data = block;
    out->length = static_cast<uint32_t>(n);
    out->release = &releaseOwned;
    out->ctx = block;
}

static void emptyF64(bronze_native_buffer* out) { (void)out; }

// Fills a transfer descriptor and THEN throws through the embed API: the
// runtime must run the release (the wrap never happens) and the program
// must see the TypeError.
static void throwAfterFill(bronze_native_buffer* out) {
    auto* block = static_cast<uint16_t*>(std::malloc(4 * sizeof(uint16_t)));
    for (int i = 0; i < 4; ++i) block[i] = static_cast<uint16_t>(i);
    ++g_bufMade;
    out->data = block;
    out->length = 4;
    out->release = &releaseOwned;
    out->ctx = block;
    bronze::embed::throwTypeError("filled, then refused");
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
// A method answering a typed array in copy mode from a per-call scratch.
static double g_agentStats[2];
static void agentStats(void* self, bronze_native_buffer* out) {
    auto* a = static_cast<Agent*>(self);
    g_agentStats[0] = a->hp;
    g_agentStats[1] = a->id;
    out->data = g_agentStats;
    out->length = 2;
}

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
           fn("nt.buf.rangeF32", reinterpret_cast<void*>(&rangeF32), "f32[]", {"i32"}) &&
           fn("nt.buf.squaresI32", reinterpret_cast<void*>(&squaresI32), "i32[]", {"i32"}) &&
           fn("nt.buf.ownedU8", reinterpret_cast<void*>(&ownedU8), "u8[]", {"i32"}) &&
           fn("nt.buf.empty", reinterpret_cast<void*>(&emptyF64), "f64[]", {}) &&
           fn("nt.buf.throwAfterFill", reinterpret_cast<void*>(&throwAfterFill), "u16[]", {}) &&
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
           member(NativeKind::Method, "nt.Agent", "nt.Agent.stats",
                  reinterpret_cast<void*>(&agentStats), "f64[]", {}) &&
           fn("nt.peek", reinterpret_cast<void*>(&peek), "f64", {"nt.Agent"}) &&
           (!withAbsent || fn("nt.absent.ping", reinterpret_cast<void*>(&absentPing), "f64", {}));
}

}  // namespace nt_natives
