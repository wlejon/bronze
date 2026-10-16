// The runtime's PER-THREAD state: the heap, the non-moving arena, the root
// shapes, the property-key INTERN table, and the caches and module spans whose
// entries are heap Values and therefore need root sources. All of it lives in
// this one translation unit so the collector's roots never depend on cross-TU
// static initialization order (rt_state.h).
//
// Per-thread, not per-process: every mutable table here is thread_local, so
// each OS thread that touches the runtime gets a whole runtime of its own —
// its own heap, its own shapes, its own interning. Threads share NOTHING
// mutable; a Value is meaningful only on the thread whose heap it points
// into. Generated code reaches the same per-thread state through its own
// seam, the bronze_tls_block its prologue fetches (bronze_abi.h), so a
// compiled module runs against whichever thread's runtime ran its entry.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>


#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/fn_source.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/iterator.h"
#include "runtime/module_instance.h"
#include "runtime/module_registry.h"
#include "runtime/native_registry.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/realm.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

// Root shapes for intrinsics and builtins. Shapes are immortal and builtins must
// stay rooted across collections; this table is what the heap's first root source walks.
static thread_local std::vector<Shape*> g_rootShapes;

// User-created prototype root shapes (e.g. from Object.create(proto) or setPrototypeOf).
// Swept after collections so dead prototype objects and their subgraphs can be collected.
static thread_local std::vector<Shape*> g_userPrototypeShapes;

static void registerThreadRootSources(Heap& heap);

static const std::thread::id g_mainThreadId = std::this_thread::get_id();
static thread_local Heap* g_threadHeap = nullptr;
static thread_local NonMovingArena* g_threadArena = nullptr;

static void releaseThreadDynamicModules();

namespace {

struct ThreadRuntimeTeardown {
    bool armed = false;
    ~ThreadRuntimeTeardown() {
        if (!armed || std::this_thread::get_id() == g_mainThreadId) return;
        rtTeardownThreadState();
    }
};

thread_local ThreadRuntimeTeardown g_threadTeardown;

}  // namespace

void rtArmThreadTeardown() {
    g_threadTeardown.armed = true;
}

// The thread's heap, bound as the thread's heap for generated code and for
// brass's runtime (Heap::bind_thread) the moment it exists, so that every
// brass interpreter on the thread registers its frames with it and allocates
// on it.
Heap& rtHeap() {
    if (!g_threadHeap) {
        rtArmThreadTeardown();
        g_threadHeap = new Heap();
        g_threadHeap->bind_thread();
        registerThreadRootSources(*g_threadHeap);
    }
    return *g_threadHeap;
}

NonMovingArena& rtArena() {
    if (!g_threadArena) {
        rtArmThreadTeardown();
        g_threadArena = new NonMovingArena();
    }
    return *g_threadArena;
}

Shape* rtNewRootShape(Value proto) {
    Shape* root = Shape::createRoot(rtArena(), proto);
    g_rootShapes.push_back(root);
    return root;
}

Shape* rtRootShapeForPrototype(Value proto) {
    // Memoized, without permanently rooting user prototypes. User prototype
    // root shapes live in `g_userPrototypeShapes`, whose prototype slots a full
    // collection visits WEAKLY: a prototype stays alive while an object built
    // on its shape does (heap_trace.cpp traces it from each), and its shape
    // leaves the table once it dies.
    for (Shape* root : g_userPrototypeShapes) {
        if (root->prototype.rawBits() == proto.rawBits()) return root;
    }
    Shape* root = Shape::createRoot(rtArena(), proto);
    g_userPrototypeShapes.push_back(root);
    return root;
}

size_t rtUserPrototypeShapeCount() {
    return g_userPrototypeShapes.size();
}

// The prototype slots of the user root shapes. A minor collection holds them
// strongly (an old object built on one is not rescanned by it, so its young
// prototype has no other visit that would move it); a full collection holds
// them weakly and clears a dead one's slot to undefined, and the sweep below
// then drops that shape.
static void visitUserPrototypeShapes(brass::gc::Tracer& t) {
    const bool full = t.purpose() == brass::gc::Tracer::Purpose::Full;
    const uint64_t cleared = Value::fromUndefined().rawBits();
    for (Shape* root : g_userPrototypeShapes) {
        auto* slot = reinterpret_cast<uint64_t*>(&root->prototype);
        if (full) t.visit_weak(slot, cleared);
        else t.visit(slot);
    }
}

static void sweepUserPrototypeShapes() {
    if (!rtHeap().collecting_full()) return;
    std::erase_if(g_userPrototypeShapes, [](Shape* root) { return root->prototype.isUndefined(); });
}

static thread_local Shape* g_plainObjectShape = nullptr;

Shape* rtPlainObjectShape() {
    if (!g_plainObjectShape) {
        Shape* shape = Shape::createRoot(rtArena(), rtObjectPrototype());
        g_rootShapes.push_back(shape);
        g_plainObjectShape = shape;
        // Published into this thread's ABI block for the inline object
        // creation fast path; shapes are arena storage, immortal and
        // non-moving, so the raw pointer never goes stale.
        bronze_tls_block_addr()->plain_shape = reinterpret_cast<uint64_t>(shape);
    }
    return g_plainObjectShape;
}

Shape* rtCurrentPlainObjectShape() {
    return g_plainObjectShape;
}

void rtRegisterRootShape(Shape* shape) {
    if (shape) {
        g_rootShapes.push_back(shape);
    }
}

// ---- ABI pins ---------------------------------------------------------------
//
// Generated code open-codes the boxing constants and the object test of the
// inline property fast path, so every constant it uses is pinned against the
// value model here.

static_assert(Value::fromUndefined().rawBits() == BRONZE_ABI_UNDEFINED_BITS,
              "BRONZE_ABI_UNDEFINED_BITS in bronze_abi.h has drifted from the value model");
static_assert(Value::fromNull().rawBits() == BRONZE_ABI_NULL_BITS,
              "BRONZE_ABI_NULL_BITS in bronze_abi.h has drifted from the value model");
static_assert(Value::fromBool(false).rawBits() == BRONZE_ABI_FALSE_BITS,
              "BRONZE_ABI_FALSE_BITS in bronze_abi.h has drifted from the value model");
static_assert(Value::fromBool(true).rawBits() == BRONZE_ABI_TRUE_BITS,
              "BRONZE_ABI_TRUE_BITS in bronze_abi.h has drifted from the value model");
static_assert(kTagShift == BRONZE_ABI_VALUE_TAG_SHIFT);
static_assert(kPayloadMask == BRONZE_ABI_VALUE_PAYLOAD_MASK);
static_assert(static_cast<uint16_t>(Tag::Object) == BRONZE_ABI_TAG_OBJECT);
static_assert(static_cast<uint16_t>(Tag::Bool) == BRONZE_ABI_TAG_BOOL);
static_assert(static_cast<uint16_t>(Tag::Int32) == BRONZE_ABI_TAG_INT32);
static_assert(kCanonicalNaNBits == BRONZE_ABI_CANONICAL_NAN_BITS);
static_assert(kNumberMaxBits == BRONZE_ABI_NUMBER_MAX_BITS);

// The inline fast path loads HeapObjectHeader::flags (offset 2) and
// ObjectHeader::shape (offsets 8..15) from any Object-tagged pointer BEFORE
// it knows which kind of object it has, so every Object-tagged allocation
// must be at least that large. An ArrayBuffer of zero bytes is the smallest.
static_assert(sizeof(ObjectHeader) - sizeof(HeapObjectHeader) >= BRONZE_ABI_OBJ_MIN_PAYLOAD);
static_assert(sizeof(ArrayHeader) - sizeof(HeapObjectHeader) >= BRONZE_ABI_OBJ_MIN_PAYLOAD);
static_assert(sizeof(FunctionHeader) - sizeof(HeapObjectHeader) >= BRONZE_ABI_OBJ_MIN_PAYLOAD);
static_assert(sizeof(TypedArrayHeader) - sizeof(HeapObjectHeader) >= BRONZE_ABI_OBJ_MIN_PAYLOAD);
static_assert(sizeof(ArrayBufferHeader) - sizeof(HeapObjectHeader) >= BRONZE_ABI_OBJ_MIN_PAYLOAD);

// ---- Caches with heap Values ------------------------------------------------

struct SingletonEntry {
    bronze_fn_code code{nullptr};
    Value value{Value::fromUndefined()};
    uint64_t epoch{0};
};

// One {code, value} entry of a module's fn-singleton table. The layout is the
// ABI's, because generated code reads the two words inline; this declaration
// exists so the runtime's writes and the collector's walk go through the same
// description the fast path does.
struct FnSingletonSlot {
    bronze_fn_code code{nullptr};
    Value value{Value::fromUndefined()};
};
static_assert(sizeof(FnSingletonSlot) == BRONZE_ABI_FNSLOT_SIZE);
static_assert(offsetof(FnSingletonSlot, code) == BRONZE_ABI_FNSLOT_CODE_OFFSET);
static_assert(offsetof(FnSingletonSlot, value) == BRONZE_ABI_FNSLOT_VALUE_OFFSET);

static_assert(sizeof(Value) == sizeof(uint64_t),
              "module cache spans are registered as raw u64 cells");

template <typename Cell>
struct ModuleSpan {
    Cell* cells;
    uint64_t count;
    uint64_t epoch;
};

struct DynamicModule {
    uint64_t epoch{0};
    std::shared_ptr<void> owner;
    std::vector<const void*> codeTokens;
    std::atomic<bool> marked{false};
};

struct ThreadModuleState {
    std::vector<SingletonEntry> functionSingletons;
    std::unordered_map<void*, size_t> functionSingletonIndex;
    std::vector<ModuleSpan<Value>> moduleValueCells;
    std::vector<ModuleSpan<FnSingletonSlot>> moduleFnSlots;
    std::vector<ModuleSpan<Value>> globalCacheSpans;
    uint64_t moduleEpochCounter = 0;
    uint64_t currentModuleEpoch = 0;
    std::vector<uint64_t> activeModuleEpochs;
    std::vector<std::unique_ptr<DynamicModule>> dynamicModules;
    std::unordered_map<const void*, DynamicModule*> codeToModule;
    std::vector<const void*> activeTokens;
};

static thread_local ThreadModuleState* g_threadModules = nullptr;
static std::atomic<uint32_t> g_totalDynamicModules{0};
static std::mutex g_heapStateMu;
static std::unordered_map<const brass::gc::Heap*, ThreadModuleState*> g_heapToModuleState;
static std::atomic<uint64_t> g_heapStateGeneration{1};

static ThreadModuleState& threadModules() {
    if (!g_threadModules) {
        rtArmThreadTeardown();
        g_threadModules = new ThreadModuleState();
    }
    return *g_threadModules;
}

static ThreadModuleState* moduleStateForGcHeap(const brass::gc::Heap& gcHeap) noexcept {
    if (g_threadHeap && &g_threadHeap->gc() == &gcHeap && g_threadModules) {
        return g_threadModules;
    }
    static thread_local const brass::gc::Heap* t_cachedHeap = nullptr;
    static thread_local ThreadModuleState* t_cachedState = nullptr;
    static thread_local uint64_t t_cachedGen = 0;
    const uint64_t gen = g_heapStateGeneration.load(std::memory_order_acquire);
    if (t_cachedGen == gen && t_cachedHeap == &gcHeap) {
        return t_cachedState;
    }
    std::lock_guard<std::mutex> lock(g_heapStateMu);
    auto it = g_heapToModuleState.find(&gcHeap);
    t_cachedHeap = &gcHeap;
    t_cachedState = (it != g_heapToModuleState.end()) ? it->second : nullptr;
    t_cachedGen = g_heapStateGeneration.load(std::memory_order_relaxed);
    return t_cachedState;
}

void rtInvalidateGlobalCaches() {
    if (!g_threadModules) return;
    for (const auto& span : g_threadModules->globalCacheSpans) {
        for (uint64_t i = 0; i < span.count; ++i) span.cells[i] = Value::fromHole();
    }
}

uint64_t rtBeginModuleEpoch() {
    ThreadModuleState& s = threadModules();
    const uint64_t epoch = ++s.moduleEpochCounter;
    s.activeModuleEpochs.push_back(epoch);
    s.currentModuleEpoch = epoch;
    return epoch;
}

void rtEndModuleEpoch(uint64_t epoch) {
    if (epoch == 0 || !g_threadModules) return;
    ThreadModuleState& s = *g_threadModules;
    if (auto it = std::find(s.activeModuleEpochs.rbegin(), s.activeModuleEpochs.rend(), epoch);
        it != s.activeModuleEpochs.rend()) {
        s.activeModuleEpochs.erase(std::next(it).base());
    }
    s.currentModuleEpoch = s.activeModuleEpochs.empty() ? 0 : s.activeModuleEpochs.back();
}

uint64_t rtCurrentModuleEpoch() noexcept {
    return g_threadModules ? g_threadModules->currentModuleEpoch : 0;
}

static void dropModuleSpansAndSingletons(ThreadModuleState& s, uint64_t epoch) {
    for (const auto& span : s.moduleFnSlots) {
        if (span.epoch != epoch) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            if (span.cells[i].code) {
                s.functionSingletonIndex.erase(reinterpret_cast<void*>(span.cells[i].code));
            }
        }
    }
    for (const auto& entry : s.functionSingletons) {
        if (entry.epoch == epoch) {
            s.functionSingletonIndex.erase(reinterpret_cast<void*>(entry.code));
        }
    }
    std::erase_if(s.functionSingletons, [epoch, &s](const SingletonEntry& entry) {
        return entry.epoch == epoch ||
               !s.functionSingletonIndex.contains(reinterpret_cast<void*>(entry.code));
    });
    for (size_t i = 0; i < s.functionSingletons.size(); ++i) {
        s.functionSingletonIndex[reinterpret_cast<void*>(s.functionSingletons[i].code)] = i;
    }
    std::erase_if(s.moduleValueCells,
                  [epoch](const ModuleSpan<Value>& sp) { return sp.epoch == epoch; });
    std::erase_if(s.moduleFnSlots,
                  [epoch](const ModuleSpan<FnSingletonSlot>& sp) { return sp.epoch == epoch; });
    std::erase_if(s.globalCacheSpans,
                  [epoch](const ModuleSpan<Value>& sp) { return sp.epoch == epoch; });
    rtDropModuleFnSources(epoch);
}

void rtDropModuleEpoch(uint64_t epoch) {
    if (epoch == 0 || !g_threadModules) return;
    ThreadModuleState& s = *g_threadModules;
    dropModuleSpansAndSingletons(s, epoch);
    std::erase_if(s.dynamicModules, [epoch, &s](const std::unique_ptr<DynamicModule>& mod) {
        if (mod->epoch != epoch) return false;
        for (const void* tok : mod->codeTokens) s.codeToModule.erase(tok);
        g_totalDynamicModules.fetch_sub(1, std::memory_order_relaxed);
        return true;
    });
    rtEndModuleEpoch(epoch);
}

void rtRegisterDynamicModule(uint64_t epoch, std::shared_ptr<void> owner,
                             const std::vector<const void*>& codeTokens) {
    if (epoch == 0 || !owner) return;
    ThreadModuleState& s = threadModules();
    auto mod = std::make_unique<DynamicModule>();
    mod->epoch = epoch;
    mod->owner = std::move(owner);
    mod->codeTokens = codeTokens;
    DynamicModule* raw = mod.get();
    for (const void* tok : raw->codeTokens) {
        if (tok) s.codeToModule[tok] = raw;
    }
    s.dynamicModules.push_back(std::move(mod));
    g_totalDynamicModules.fetch_add(1, std::memory_order_relaxed);
}

bool rtHasDynamicModules() noexcept {
    return g_totalDynamicModules.load(std::memory_order_relaxed) != 0;
}

size_t rtDynamicModuleCount() noexcept {
    return g_threadModules ? g_threadModules->dynamicModules.size() : 0;
}

void rtPushActiveDynamicToken(const void* token) noexcept {
    if (!g_threadModules || !token) return;
    g_threadModules->activeTokens.push_back(token);
}

void rtPopActiveDynamicToken() noexcept {
    if (!g_threadModules || g_threadModules->activeTokens.empty()) return;
    g_threadModules->activeTokens.pop_back();
}

static void markDynamicModuleSpans(ThreadModuleState& s, DynamicModule* mod, brass::gc::Tracer& t) {
    if (!mod || mod->marked.load(std::memory_order_relaxed)) return;
    if (mod->marked.exchange(true, std::memory_order_acq_rel)) return;
    const uint64_t epoch = mod->epoch;
    for (auto& entry : s.functionSingletons) {
        if (entry.epoch == epoch) {
            t.visit(reinterpret_cast<uint64_t*>(&entry.value));
        }
    }
    for (const auto& span : s.moduleFnSlots) {
        if (span.epoch != epoch) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            if (span.cells[i].code) {
                t.visit(reinterpret_cast<uint64_t*>(&span.cells[i].value));
            }
        }
    }
    for (const auto& span : s.moduleValueCells) {
        if (span.epoch != epoch) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            t.visit(reinterpret_cast<uint64_t*>(&span.cells[i]));
        }
    }
    for (const auto& span : s.globalCacheSpans) {
        if (span.epoch != epoch) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            t.visit(reinterpret_cast<uint64_t*>(&span.cells[i]));
        }
    }
}

void rtTraceDynamicModuleToken(const void* token, brass::gc::Tracer& t) {
    if (!token || t.purpose() != brass::gc::Tracer::Purpose::Full) return;
    ThreadModuleState* s = moduleStateForGcHeap(t.heap());
    if (!s || s->dynamicModules.empty()) return;
    auto it = s->codeToModule.find(token);
    if (it != s->codeToModule.end()) {
        markDynamicModuleSpans(*s, it->second, t);
    }
}

static bool isEpochActiveOnStack(const ThreadModuleState& s, uint64_t epoch) noexcept {
    if (epoch == 0) return true;
    for (uint64_t active : s.activeModuleEpochs) {
        if (active == epoch) return true;
    }
    return false;
}

static bool isDynamicModuleEpoch(const ThreadModuleState& s, uint64_t epoch) noexcept {
    if (epoch == 0) return false;
    for (const auto& mod : s.dynamicModules) {
        if (mod->epoch == epoch) return true;
    }
    return false;
}

static void visitModuleAndCacheRoots(brass::gc::Tracer& t) {
    if (!g_threadModules) return;
    ThreadModuleState& s = *g_threadModules;
    const bool full = (t.purpose() == brass::gc::Tracer::Purpose::Full) && !s.dynamicModules.empty();
    if (full) {
        for (const auto& mod : s.dynamicModules) {
            mod->marked.store(false, std::memory_order_relaxed);
        }
        for (const auto& mod : s.dynamicModules) {
            if (isEpochActiveOnStack(s, mod->epoch)) {
                markDynamicModuleSpans(s, mod.get(), t);
            }
        }
        for (const void* tok : s.activeTokens) {
            if (auto it = s.codeToModule.find(tok); it != s.codeToModule.end()) {
                markDynamicModuleSpans(s, it->second, t);
            }
        }
    }
    for (auto& entry : s.functionSingletons) {
        if (!full || !isDynamicModuleEpoch(s, entry.epoch)) {
            t.visit(reinterpret_cast<uint64_t*>(&entry.value));
        }
    }
    for (const auto& span : s.moduleFnSlots) {
        if (full && isDynamicModuleEpoch(s, span.epoch)) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            if (span.cells[i].code) {
                t.visit(reinterpret_cast<uint64_t*>(&span.cells[i].value));
            }
        }
    }
    for (const auto& span : s.moduleValueCells) {
        if (full && isDynamicModuleEpoch(s, span.epoch)) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            t.visit(reinterpret_cast<uint64_t*>(&span.cells[i]));
        }
    }
    for (const auto& span : s.globalCacheSpans) {
        if (full && isDynamicModuleEpoch(s, span.epoch)) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            t.visit(reinterpret_cast<uint64_t*>(&span.cells[i]));
        }
    }
}

static void sweepDynamicModules() {
    if (!g_threadHeap || !g_threadHeap->collecting_full() || !g_threadModules) return;
    ThreadModuleState& s = *g_threadModules;
    if (s.dynamicModules.empty()) return;
    std::vector<std::shared_ptr<void>> retiredOwners;
    std::erase_if(s.dynamicModules, [&s, &retiredOwners](const std::unique_ptr<DynamicModule>& mod) {
        if (mod->marked.load(std::memory_order_relaxed) || isEpochActiveOnStack(s, mod->epoch)) {
            return false;
        }
        for (const void* tok : mod->codeTokens) s.codeToModule.erase(tok);
        dropModuleSpansAndSingletons(s, mod->epoch);
        g_totalDynamicModules.fetch_sub(1, std::memory_order_relaxed);
        retiredOwners.push_back(std::move(mod->owner));
        return true;
    });
}

// Globals an embedding host registered (host_globals.h). A vector of pairs
// rather than a map: registration happens a handful of times at host startup,
// and a lookup is a read `bronze_global_get` reaches only for a name the
// builtin ladder did not answer.
static thread_local std::vector<std::pair<std::string, Value>> g_hostGlobals;
// The pending inits of lazy host globals (host_globals.h), whose entries in
// g_hostGlobals hold the hole until forced.
static thread_local std::vector<std::pair<std::string, std::function<void()>>> g_lazyHostGlobals;

static void registerThreadRootSources(Heap& heap) {
    {
        ThreadModuleState& s = threadModules();
        std::lock_guard<std::mutex> lock(g_heapStateMu);
        g_heapToModuleState[&heap.gc()] = &s;
        g_heapStateGeneration.fetch_add(1, std::memory_order_release);
    }
    heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (Shape* root : g_rootShapes) visit(root->prototype);
    });
    heap.add_tracer_source(visitUserPrototypeShapes);
    heap.add_tracer_source(visitModuleAndCacheRoots);
    heap.add_post_collection_hook([]() {
        sweepUserPrototypeShapes();
        sweepDynamicModules();
    });
    heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (auto& entry : g_hostGlobals) visit(entry.second);
        rtVisitArrayMethodRoots(visit);
        rtVisitRealmRoots(visit);
    });
}

void rtRegisterHostGlobal(const std::string& name, Value value) {
    // A name is a native or a host global, never both: `f(1)` compiled as a
    // direct native call while `f` read as a host value would be two
    // different things under one name. Refused loudly here and in
    // rtRegisterNative, whichever comes second.
    if (rtNativeBareNameRegistered(name)) {
        fatal(("registerGlobal(\"" + name +
               "\"): the name is a registered native (embed::registerNative); a name is a "
               "native or a host global, not both")
                  .c_str());
    }
    // Before the write, unconditionally: a REPLACED name has cells holding
    // the old value, and a NEW name can have cells too — `performance` is a
    // builtin until a host provides its own, and every module that read the
    // builtin cached it.
    rtInvalidateGlobalCaches();
    // A pending lazy name registered for real: its init has nothing left to
    // do for it (host_globals.h).
    for (auto it = g_lazyHostGlobals.begin(); it != g_lazyHostGlobals.end(); ++it) {
        if (it->first == name) {
            g_lazyHostGlobals.erase(it);
            break;
        }
    }
    for (auto& entry : g_hostGlobals) {
        if (entry.first == name) {
            entry.second = value;
            rtDefineHostGlobalOnLiveRealms(name, value);
            return;
        }
    }
    g_hostGlobals.emplace_back(name, value);
    // The registry answers the BARE name; a realm's global object is what
    // `globalThis[name]`, `name in globalThis` and every reflective walk read.
    // Both, or a host global is invisible to feature detection (realm.h states
    // the rule and why a builtin name is not written).
    rtDefineHostGlobalOnLiveRealms(name, value);
}

bool rtHostGlobalLookup(const std::string& name, Value& out) {
    for (const auto& entry : g_hostGlobals) {
        if (entry.first == name) {
            if (entry.second.isHole()) {
                rtForceLazyHostGlobal(name);
                // The init may have registered names, growing the vector:
                // read the entry again.
                for (const auto& again : g_hostGlobals) {
                    if (again.first == name) {
                        out = again.second.isHole() ? Value::fromUndefined() : again.second;
                        return true;
                    }
                }
                out = Value::fromUndefined();
                return true;
            }
            out = entry.second;
            return true;
        }
    }
    return false;
}

bool rtHostGlobalRegistered(const std::string& name) {
    for (const auto& entry : g_hostGlobals) {
        if (entry.first == name) return true;
    }
    return false;
}

void rtRegisterLazyHostGlobal(const std::string& name, std::function<void()> init) {
    if (rtNativeBareNameRegistered(name)) {
        fatal(("registerLazyGlobal(\"" + name +
               "\"): the name is a registered native (embed::registerNative); a name is a "
               "native or a host global, not both")
                  .c_str());
    }
    // Already registered for real: there is nothing to defer.
    for (const auto& entry : g_hostGlobals) {
        if (entry.first == name && !entry.second.isHole()) return;
    }
    bool replaced = false;
    for (auto& pending : g_lazyHostGlobals) {
        if (pending.first == name) {
            pending.second = std::move(init);
            replaced = true;
            break;
        }
    }
    if (!replaced) g_lazyHostGlobals.emplace_back(name, std::move(init));
    if (!rtHostGlobalRegistered(name)) {
        rtInvalidateGlobalCaches();
        g_hostGlobals.emplace_back(name, Value::fromHole());
        rtDefineLazyHostGlobalOnLiveRealms(name);
    }
}

static thread_local uint32_t g_lazyInitDepth = 0;

bool rtInLazyGlobalInit() { return g_lazyInitDepth != 0; }

void rtForceLazyHostGlobal(const std::string& name) {
    std::function<void()> init;
    for (auto it = g_lazyHostGlobals.begin(); it != g_lazyHostGlobals.end(); ++it) {
        if (it->first == name) {
            init = std::move(it->second);
            g_lazyHostGlobals.erase(it);
            break;
        }
    }
    if (!init) return;
    // The name's accessor goes first, so that the init's module may assign
    // the global as a plain data property; registered `undefined` until the
    // init registers what it made.
    rtRegisterHostGlobal(name, Value::fromUndefined());
    rtRunAsLazyInit(init);
}

void rtRunAsLazyInit(const std::function<void()>& init) {
    ++g_lazyInitDepth;
    struct Leave {
        ~Leave() { --g_lazyInitDepth; }
    } leave;
    init();
}

static LazyGlobalAccessorHook g_lazyGlobalAccessorHook = nullptr;

void rtSetLazyGlobalAccessorHook(LazyGlobalAccessorHook hook) { g_lazyGlobalAccessorHook = hook; }

LazyGlobalAccessorHook rtLazyGlobalAccessorHook() { return g_lazyGlobalAccessorHook; }

// The host's CreateDynamicFunction, if it has one (host_globals.h says why the
// runtime offers the seam at all). Not a root source: whatever the host closed
// over is the host's to keep alive, and the Values it hands back per call are
// rooted by the caller like any other builtin result.
static thread_local DynamicFunctionHost g_dynamicFunctionHost;
static DynamicFunctionHost g_defaultDynamicFunctionHost;

void rtSetDynamicFunctionHost(DynamicFunctionHost host) {
    g_dynamicFunctionHost = std::move(host);
}

void rtSetDefaultDynamicFunctionHost(DynamicFunctionHost host) {
    g_defaultDynamicFunctionHost = std::move(host);
}

const DynamicFunctionHost& rtDynamicFunctionHost() {
    if (g_dynamicFunctionHost) return g_dynamicFunctionHost;
    return g_defaultDynamicFunctionHost;
}

// The host's eval, if it has one — the same seam, for `Function`'s sibling.
static thread_local DynamicEvalHost g_dynamicEvalHost;
static DynamicEvalHost g_defaultDynamicEvalHost;

void rtSetDynamicEvalHost(DynamicEvalHost host) { g_dynamicEvalHost = std::move(host); }

void rtSetDefaultDynamicEvalHost(DynamicEvalHost host) {
    g_defaultDynamicEvalHost = std::move(host);
}

const DynamicEvalHost& rtDynamicEvalHost() {
    if (g_dynamicEvalHost) return g_dynamicEvalHost;
    return g_defaultDynamicEvalHost;
}

// The host's `import()`, if it has one — the seam for the specifier the module
// graph could not read at compile time.
static thread_local DynamicImportHost g_dynamicImportHost;

void rtSetDynamicImportHost(DynamicImportHost host) { g_dynamicImportHost = std::move(host); }

const DynamicImportHost& rtDynamicImportHost() { return g_dynamicImportHost; }

// 10.2.9 and 10.2.10, as the one place a function object's two own data
// properties are filled in. `BRONZE_ABI_FN_NAME_NONE` leaves both absent, which
// is what a native builtin gets — the header's comment says why that is not the
// same as an empty name.
void rtSetFunctionNameAndLength(FunctionHeader* fn, uint32_t nameKey, uint32_t length) {
    if (nameKey == BRONZE_ABI_FN_NAME_NONE) return;
    StringHeader* header = rtKeyHeader(nameKey);
    if (!header) fatal("a function created with an unregistered name key index");
    fn->name = header;
    fn->length = length;
}

extern "C" {

void bronze_register_value_cells(uint64_t* cells, uint64_t count) {
    if (!cells || count == 0) return;
    ThreadModuleState& s = threadModules();
    s.moduleValueCells.push_back(
        {reinterpret_cast<Value*>(cells), count, s.currentModuleEpoch});
}

void bronze_register_fn_slots(uint64_t* cells, uint64_t count) {
    if (!cells || count == 0) return;
    ThreadModuleState& s = threadModules();
    s.moduleFnSlots.push_back(
        {reinterpret_cast<FnSingletonSlot*>(cells), count, s.currentModuleEpoch});
}

void bronze_register_method_ic_cells(uint64_t* icTable, const uint64_t* siteIndexes,
                                     uint64_t count) {
    if (!icTable || !siteIndexes || count == 0) return;
    ThreadModuleState& s = threadModules();
    constexpr uint64_t kSiteWords = BRONZE_ABI_IC_SITE_SIZE / sizeof(uint64_t);
    for (uint64_t k = 0; k < count; ++k) {
        uint64_t* site = icTable + siteIndexes[k] * kSiteWords;
        s.moduleValueCells.push_back(
            {reinterpret_cast<Value*>(site + BRONZE_ABI_METHOD_IC_ENV_WORD), 1,
             s.currentModuleEpoch});
        s.moduleValueCells.push_back(
            {reinterpret_cast<Value*>(site + BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD), 1,
             s.currentModuleEpoch});
    }
}

}  // extern "C"

static thread_local bool g_envMethodIcEnabled = true;

void rtSetEnvMethodIcEnabled(bool enabled) { g_envMethodIcEnabled = enabled; }
bool rtEnvMethodIcEnabled() noexcept { return g_envMethodIcEnabled; }

static thread_local bool g_exoticMethodIcEnabled = true;

void rtSetExoticMethodIcEnabled(bool enabled) { g_exoticMethodIcEnabled = enabled; }
bool rtExoticMethodIcEnabled() noexcept { return g_exoticMethodIcEnabled; }

static thread_local bool g_polyMethodIcEnabled = true;

void rtSetPolyMethodIcEnabled(bool enabled) { g_polyMethodIcEnabled = enabled; }
bool rtPolyMethodIcEnabled() noexcept { return g_polyMethodIcEnabled; }

// ---- the interned-native table, as the memo in front of it sees it --------

uint32_t rtFunctionSingletonIndexOf(bronze_fn_code code) {
    if (!g_threadModules) return UINT32_MAX;
    const auto& idxMap = g_threadModules->functionSingletonIndex;
    if (auto it = idxMap.find(reinterpret_cast<void*>(code)); it != idxMap.end()) {
        return static_cast<uint32_t>(it->second);
    }
    return UINT32_MAX;
}

Value rtFunctionSingletonAt(uint32_t index, bronze_fn_code expect) {
    if (!g_threadModules || index >= g_threadModules->functionSingletons.size()) {
        return Value::fromUndefined();
    }
    const auto& entry = g_threadModules->functionSingletons[index];
    if (entry.code != expect) return Value::fromUndefined();
    return entry.value;
}

static std::mutex g_nativeDisplayNamesMu;
static std::unordered_map<const void*, std::string> g_nativeDisplayNames;

static const void* resolveJumpThunk(const void* p) {
    if (!p) return nullptr;
#if defined(_WIN32) && defined(_M_X64)
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    if (b[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
        return b + 5 + rel;
    }
#endif
    return p;
}

void rtRegisterNativeDisplayName(const void* code, const char* displayName) {
    if (!code || !displayName) return;
    std::lock_guard<std::mutex> lock(g_nativeDisplayNamesMu);
    g_nativeDisplayNames[code] = displayName;
    const void* target = resolveJumpThunk(code);
    if (target && target != code) {
        g_nativeDisplayNames[target] = displayName;
    }
}

const char* rtGetNativeDisplayName(const void* code) {
    if (!code) return nullptr;
    std::lock_guard<std::mutex> lock(g_nativeDisplayNamesMu);
    auto it = g_nativeDisplayNames.find(code);
    if (it != g_nativeDisplayNames.end()) {
        return it->second.c_str();
    }
    const void* target = resolveJumpThunk(code);
    if (target && target != code) {
        it = g_nativeDisplayNames.find(target);
        if (it != g_nativeDisplayNames.end()) {
            return it->second.c_str();
        }
    }
    return nullptr;
}

extern "C" {

uint64_t bronze_function_singleton(bronze_fn_code code, uint32_t arity, uint32_t length,
                                   uint32_t nameKey, uint32_t fnFlags, uint64_t* slotCell) {
    recordHelperCall("bronze_function_singleton");
    ThreadModuleState& s = threadModules();
    Value result = Value::fromUndefined();
    if (auto it = s.functionSingletonIndex.find(reinterpret_cast<void*>(code));
        it != s.functionSingletonIndex.end()) {
        result = s.functionSingletons[it->second].value;
    } else {
        FunctionHeader* fn =
            FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity, fnFlags);
        fn->env_record = Value::fromObject(fn);
        fn->header.flags = HeapKind::Function;
        rtSetFunctionNameAndLength(fn, nameKey, length);
        result = Value::fromObject(fn);
        uint64_t fnEpoch = 0;
        if ((fnFlags & BRONZE_ABI_FN_FLAG_NATIVE) == 0) {
            if (auto modIt = s.codeToModule.find(reinterpret_cast<const void*>(code));
                modIt != s.codeToModule.end()) {
                fnEpoch = modIt->second->epoch;
            } else {
                fnEpoch = s.currentModuleEpoch;
            }
        }
        s.functionSingletons.push_back(SingletonEntry{code, result, fnEpoch});
        s.functionSingletonIndex.emplace(reinterpret_cast<void*>(code),
                                         s.functionSingletons.size() - 1);
    }
    if (slotCell) {
        *reinterpret_cast<FnSingletonSlot*>(slotCell) = FnSingletonSlot{code, result};
    }
    return result.rawBits();
}

}  // extern "C"

bool rtResolveBuiltinGlobal(const std::string& keyStr, Value& out) {
    if (keyStr == "Math") {
        out = rtMathObject();
    } else if (keyStr == "Object") {
        out = rtObjectNamespace();
    } else if (keyStr == "Function") {
        out = rtFunctionConstructorObject();
    } else if (keyStr == "JSON") {
        out = rtJsonNamespace();
    } else if (keyStr == "globalThis") {
        out = rtGlobalThisObject();
    } else if (keyStr == "performance") {
        out = rtPerformanceNamespace();
    } else if (keyStr == "Reflect") {
        out = rtReflectNamespace();
    } else if (keyStr == "Date") {
        out = rtDateConstructor();
    } else if (keyStr == "Intl") {
        out = rtIntlNamespace();
    } else if (keyStr == "Symbol") {
        out = rtSymbolFunction();
    } else if (Value bigint = rtBigIntConstructor(keyStr); bigint.isObject()) {
        out = bigint;
    } else if (Value regexp = rtRegExpConstructor(keyStr); regexp.isObject()) {
        out = regexp;
    } else if (Value iterator = rtIteratorConstructor(keyStr); iterator.isObject()) {
        out = iterator;
    } else if (Value collection = rtMapConstructor(keyStr); collection.isObject()) {
        out = collection;
    } else if (Value weak = rtWeakCollectionConstructor(keyStr); weak.isObject()) {
        out = weak;
    } else if (Value weakRef = rtWeakRefConstructor(keyStr); weakRef.isObject()) {
        out = weakRef;
    } else if (Value typed = rtTypedArrayConstructor(keyStr); typed.isObject()) {
        out = typed;
    } else if (Value shared = rtSharedArrayBufferConstructor(keyStr); shared.isObject()) {
        out = shared;
    } else if (keyStr == "Atomics") {
        out = rtAtomicsObject();
    } else if (Value dataView = rtDataViewConstructor(keyStr); dataView.isObject()) {
        out = dataView;
    } else if (Value global = rtGlobalConstructor(keyStr); global.isObject()) {
        out = global;
    } else if (Value ctor = rtErrorConstructor(keyStr); ctor.isObject()) {
        out = ctor;
    } else if (Value promise = rtPromiseConstructor(keyStr); promise.isObject()) {
        out = promise;
    } else if (Value numeric = rtGlobalNumericFunction(keyStr); numeric.isObject()) {
        out = numeric;
    } else if (Value moduleIntrinsic = rtModuleRegistryIntrinsic(keyStr);
               moduleIntrinsic.isObject()) {
        out = moduleIntrinsic;
    } else {
        return false;
    }
    return true;
}

const std::vector<std::pair<std::string, Value>>& rtHostGlobalEntries() { return g_hostGlobals; }

extern "C" {

uint64_t bronze_global_get(uint32_t keyIndex, uint64_t* cacheCell) {
    recordPropCall("bronze_global_get", keyIndex, nullptr);
    const std::string& keyStr = rtKeyString(keyIndex);
    if (keyStr == "globalThis") {
        return rtGlobalThisObject().rawBits();
    }
    if (rtHostShadowsBuiltin(keyStr)) {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            return host.rawBits();
        }
    }
    Value resolved = Value::fromUndefined();
    if (!rtResolveBuiltinGlobal(keyStr, resolved)) {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            return host.rawBits();
        }
        if (Value fromGlobalObject = Value::fromUndefined();
            rtGlobalThisOwnLookup(keyStr, fromGlobalObject)) {
            return fromGlobalObject.rawBits();
        }
        fatal(("internal: no global named " + keyStr).c_str());
    }
    if (cacheCell) *cacheCell = resolved.rawBits();
    return resolved.rawBits();
}

uint64_t bronze_global_get_cached(uint32_t keyIndex, uint64_t* cells, uint64_t count,
                                  uint32_t slot) {
    recordPropCall("bronze_global_get_cached", keyIndex, nullptr);
    ThreadModuleState& s = threadModules();
    auto* valueCells = reinterpret_cast<Value*>(cells);
    if (cells && count != 0 && slot < count) {
        bool known = false;
        for (const auto& span : s.globalCacheSpans) {
            if (span.cells == valueCells) {
                known = true;
                break;
            }
        }
        if (!known) {
            uint64_t spanEpoch = s.currentModuleEpoch;
            for (const auto& vspan : s.moduleValueCells) {
                if (vspan.cells + vspan.count == valueCells) {
                    spanEpoch = vspan.epoch;
                    break;
                }
            }
            s.globalCacheSpans.push_back({valueCells, count, spanEpoch});
        }
    }
    const std::string& keyStr = rtKeyString(keyIndex);
    Value resolved = Value::fromUndefined();
    bool cacheable = false;
    if (keyStr == "globalThis") {
        resolved = rtGlobalThisObject();
        cacheable = false;
    } else if (rtHostShadowsBuiltin(keyStr) && rtHostGlobalLookup(keyStr, resolved)) {
        cacheable = true;
    } else if (rtResolveBuiltinGlobal(keyStr, resolved)) {
        cacheable = true;
    } else if (rtHostGlobalLookup(keyStr, resolved)) {
        cacheable = true;
    } else if (!rtGlobalThisOwnLookup(keyStr, resolved)) {
        fatal(("internal: no global named " + keyStr).c_str());
    }
    if (cacheable && cells && slot < count) valueCells[slot] = resolved;
    return resolved.rawBits();
}

}  // extern "C"

static void releaseThreadDynamicModules() {
    if (!g_threadModules) return;
    const size_t n = g_threadModules->dynamicModules.size();
    if (n != 0) {
        g_totalDynamicModules.fetch_sub(static_cast<uint32_t>(n), std::memory_order_relaxed);
    }
    g_threadModules->codeToModule.clear();
    g_threadModules->dynamicModules.clear();
}

void rtTeardownThreadState() {
    g_plainObjectShape = nullptr;
    bronze_tls_block_addr()->plain_shape = 0;

    if (g_threadHeap) {
        std::lock_guard<std::mutex> lock(g_heapStateMu);
        g_heapToModuleState.erase(&g_threadHeap->gc());
        g_heapStateGeneration.fetch_add(1, std::memory_order_release);
    }

    if (g_threadModules) {
        releaseThreadDynamicModules();
        delete g_threadModules;
        g_threadModules = nullptr;
    }

    if (g_threadHeap) {
        delete g_threadHeap;
        g_threadHeap = nullptr;
    }
    if (g_threadArena) {
        delete g_threadArena;
        g_threadArena = nullptr;
    }
}

}  // namespace bronze::runtime
