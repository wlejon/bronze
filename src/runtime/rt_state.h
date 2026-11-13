#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The runtime's process-wide state — the heap, the non-moving arena, the root
// shapes and the key registry the index lowering writes into.
//
// All of it is owned by ONE translation unit (rt_state.cpp), so its
// construction order is that unit's business alone. Every other runtime
// translation unit reaches it through the accessors here rather than declaring
// statics of its own, which would put the collector's roots at the mercy of
// cross-TU initialization order.

namespace bronze {
class Realm;
struct FunctionHeader;
}

namespace bronze::runtime {

namespace rt_detail {
// The thread's heap, null until its first use (rt_state.cpp). constinit: a
// read is one load off the thread pointer, with no initialization wrapper.
extern constinit thread_local Heap* t_threadHeap;
Heap& rtCreateThreadHeap();
}  // namespace rt_detail

// The calling thread's heap, created at its first use.
inline Heap& rtHeap() {
    if (Heap* heap = rt_detail::t_threadHeap) [[likely]] return *heap;
    return rt_detail::rtCreateThreadHeap();
}
NonMovingArena& rtArena();

// A root shape registered with the collector, for a builtin that needs its
// own hidden class rather than the one every `{}` literal shares. Objects of
// its tree have `inlineSlots` inline slots (Shape::createRoot).
Shape* rtNewRootShape(Value proto, uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS);

// The root shape for objects whose prototype is `proto`, memoized on the
// prototype's identity. `Object.create(p)` in a loop must not mint a hidden
// class per object — every one of them would be a shape no inline cache had
// ever seen, and each would leak an immortal arena shape.
Shape* rtRootShapeForPrototype(Value proto);
size_t rtUserPrototypeShapeCount();

// The one root shape every plain `{}` literal starts from. Per-literal root
// shapes would give two identical literals unrelated hidden classes, so a site
// seeing both would miss its inline cache every time.
Shape* rtPlainObjectShape();
Shape* rtCurrentPlainObjectShape();
void rtRegisterRootShape(Shape* shape);

// The same root for a literal whose objects have `inlineSlots` inline slots
// (clamped as Shape::clampInlineSlots does): one per capacity, so literals of
// one size share a tree. The default capacity's is rtPlainObjectShape().
Shape* rtPlainObjectShapeSized(uint32_t inlineSlots);
// Whether `root` is one of those.
bool rtIsPlainObjectRoot(const Shape* root);

// How many inline slots `new F()` gives an instance of the function F, which
// is the capacity its instance root shape is made with: what the compiler
// predicted for F's code (bronze_create_function's flag bits,
// BRONZE_ABI_FN_INLINE_SLOTS_SHIFT), the default when it predicted nothing,
// and always the default for a function whose instances are exotic objects
// (a native base: internal slots at fixed offsets).
uint32_t rtInstanceInlineSlots(const FunctionHeader* fn);
// The same for the `prototype` object the runtime makes for F: a class's
// carries its methods, so it gets the most; any other function's the default.
uint32_t rtPrototypeInlineSlots(const FunctionHeader* fn);
// Records the prediction carried by `fnFlags` for `code`, if there is one.
void rtNoteInstanceInlineSlots(const void* code, uint32_t fnFlags);

// A property key by the index lowering assigned it. The string form is for
// comparisons; the header form is the arena-interned key the property path
// uses, so a property access allocates nothing. `rtKeyHeader` is null for an
// index no `bronze_register_key_string` call ever covered.
struct KeyInfo {
    uint32_t elemIndex = UINT32_MAX;
    bool isElemIndex = false;
    bool isLength = false;
    bool isPrototype = false;
};

const std::string& rtKeyString(uint32_t index);
StringHeader* rtKeyHeader(uint32_t index);
const KeyInfo& rtKeyInfo(uint32_t index);
void rtTeardownThreadKeys();
extern "C" uint32_t bronze_register_key_string_len(const char* str, size_t len);

// Arm and execute per-thread runtime teardown on worker thread exit.
void rtArmThreadTeardown();
void rtTeardownThreadState();

// Hand the host globals registered before the thread had a realm to the
// default realm as it is made (realm.cpp; Realm::hostGlobals).
void rtAdoptPreRealmHostGlobals(::bronze::Realm& realm);
// Re-key the compiled function singletons made before the default realm to
// its slot (rt_state.cpp).
void rtAdoptPreRealmFunctionSingletons(uint32_t slot);
// The values of those registrations, for rt_state.cpp's root source.
void rtVisitPreRealmHostGlobals(const Heap::RootVisitor& visit);

uint32_t rtArrayMethodId(const std::string& key);
Value rtArrayMethodById(uint32_t id);
void rtVisitArrayMethodRoots(const Heap::RootVisitor& visit);
// The for-in key arrays rt_enumerate.cpp caches per shape: visited with the
// thread's other runtime tables, and emptied when the thread's heap is made
// (a table must never hold another heap's arrays).
void rtVisitForInArrayRoots(const Heap::RootVisitor& visit);
void rtResetForInArrayCache();
// Every module global-read cell of this thread back to a hole, so the next
// read of each resolves again: a program wrote a builtin's binding on the
// global object (rtNoteGlobalObjectWrite).
void rtInvalidateGlobalReadCells();

// Module load epochs: the unregister half of the module root spans. A host
// (embed.h's beginModuleLoad/unloadModule) brackets a module's entry with an
// epoch; every span the module registers during its entry is tagged with it,
// and dropping the epoch removes those spans — and the function singletons
// interned through the module's own fn slots — from the collector's roots.
// Epoch 0 is "no bracket": spans registered outside any bracket (a linked
// program's, a host that never unloads) are permanent, exactly as before.
// Ending an epoch returns the thread to its enclosing epoch if that epoch is
// the current one; spans registered after the entry has returned are then
// permanent (or resolved to their owning dynamic module) rather than tagged
// with a module they do not belong to.
uint64_t rtBeginModuleEpoch();
void rtEndModuleEpoch(uint64_t epoch);
// Empties every accessor and method cell that names a shape
// (accessor_cell.cpp): at a prototype-epoch bump, and before the memory a
// cell lives in or the shapes it names can go.
void rtForgetPublishedCells() noexcept;
void rtDropModuleEpoch(uint64_t epoch);
uint64_t rtCurrentModuleEpoch() noexcept;
// Spans registered between these two are permanent (epoch 0) whatever bracket
// is open around them: the host's lazily-entered wrapper modules
// (rtRunAsLazyInit) belong to the host, not to whichever program's first read
// happened to force them, and must not die when that program is unloaded.
void rtEnterPermanentModuleScope();
void rtLeavePermanentModuleScope();

// Dynamic modules (eval / Function / evalScript) whose functions or coroutines
// may escape to the heap: kept alive only while at least one FunctionHeader or
// CoroutineHeader referencing one of their code/dispatch tokens is reachable or
// actively executing on the stack, and swept on full GC once unreachable.
void rtRegisterDynamicModule(uint64_t epoch, std::shared_ptr<void> owner,
                             const std::vector<const void*>& codeTokens);
// Inline: every entry from C++ into generated code asks it (rtEnterJsIn's
// token scope), and a three.js render-list sort enters once per comparison.
extern std::atomic<uint32_t> g_totalDynamicModules;
inline bool rtHasDynamicModules() noexcept {
    return g_totalDynamicModules.load(std::memory_order_relaxed) != 0;
}
size_t rtDynamicModuleCount() noexcept;
void rtTraceDynamicModuleToken(const void* token, brass::gc::Tracer& t);
void rtPushActiveDynamicToken(const void* token) noexcept;
void rtPopActiveDynamicToken() noexcept;

struct ActiveDynamicTokenScope {
    bool pushed = false;
    explicit ActiveDynamicTokenScope(const void* token) noexcept {
        if (token && rtHasDynamicModules()) {
            rtPushActiveDynamicToken(token);
            pushed = true;
        }
    }
    ~ActiveDynamicTokenScope() {
        if (pushed) rtPopActiveDynamicToken();
    }
    ActiveDynamicTokenScope(const ActiveDynamicTokenScope&) = delete;
    ActiveDynamicTokenScope& operator=(const ActiveDynamicTokenScope&) = delete;
};

// Invalidate cached global cells across all modules on host global registration and realm switches.
void rtInvalidateGlobalCaches();

// A destroyed realm's module instances (module_instance.cpp): forget every
// GC registration (value cells, fn slots, global caches) whose cells lie in
// [begin, begin + size), before the memory is freed.
void rtDropModuleCellsWithin(const void* begin, size_t size);
// A destroyed realm's function singletons (bronze_function_singleton keeps
// one per code pointer PER REALM for compiled functions; the natives' are
// the thread's and shared).
void rtDropRealmFunctionSingletons(uint32_t realmSlot);

}  // namespace bronze::runtime
