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
#include "runtime/root_remset.h"
#include "runtime/rt_module_state.h"
#include "runtime/stack_locals.h"
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

// The module epochs THIS STACK has begun and not ended, innermost last
// (runtime/stack_locals.h): what a span registered now belongs to. Per
// stack, not per thread, since compartments interleave modules on one
// thread: a compartment's module that ends must not leave the thread's
// current epoch at another compartment's, or the spans its resumed code
// registers later (a tier-up's cells) are filed under that epoch and
// outlive their own module's code. The thread's activeModuleEpochs stays
// thread-wide: the sweep must keep every suspended stack's modules.
static thread_local std::vector<uint64_t> t_stackModuleEpochs;
static std::vector<uint64_t>& stackModuleEpochsRef() { return t_stackModuleEpochs; }
static const int g_stackModuleEpochsLocal =  // NOLINT: registered for its effect
    registerStackLocalBox<std::vector<uint64_t>, &stackModuleEpochsRef>();
static uint64_t stackModuleEpoch() noexcept {
    return t_stackModuleEpochs.empty() ? 0 : t_stackModuleEpochs.back();
}

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

// The two shape tables' remembered sets (runtime/root_remset.h): the entries
// whose prototype may be young. A root shape's prototype is written once, as
// the shape is made and before it joins its table, so the push is the only
// writer either table has.
static thread_local RootIndexSet g_youngRootShapes;
static thread_local RootIndexSet g_youngUserPrototypeShapes;

static std::string shapeTablesRemsetCheck(const brass::gc::Heap& heap);

static uint64_t* prototypeSlot(Shape* root) noexcept {
    return reinterpret_cast<uint64_t*>(&root->prototype);
}

static void pushRootShape(Shape* root) {
    g_rootShapes.push_back(root);
    if (gcHoldsYoung(root->prototype)) {
        g_youngRootShapes.add(static_cast<uint32_t>(g_rootShapes.size() - 1));
    }
}

Shape* rtNewRootShape(Value proto) {
    Shape* root = Shape::createRoot(rtArena(), proto);
    pushRootShape(root);
    return root;
}

// Every root shape's prototype, strongly: they are the builtins' and are
// never dropped. A minor pass visits only the ones that may be young.
static void visitRootShapes(brass::gc::Tracer& t) {
    auto slotAt = [](uint32_t i) { return prototypeSlot(g_rootShapes[i]); };
    if (rtRootRemsetPass(t)) {
        g_youngRootShapes.visitMinor(t, slotAt);
        return;
    }
    for (Shape* root : g_rootShapes) t.visit(prototypeSlot(root));
    // With the sets off a minor still keeps them current, so they are right
    // when switched back on.
    if (t.purpose() == brass::gc::Tracer::Purpose::Minor) g_youngRootShapes.visitMinor(t, slotAt);
}

Shape* rtRootShapeForPrototype(Value proto) {
    // Memoized, without permanently rooting user prototypes. User prototype
    // root shapes live in `g_userPrototypeShapes`, whose prototype slots a full
    // collection visits WEAKLY: a prototype stays alive while an object built
    // on its shape does (heap_trace.cpp traces it from each), and its shape
    // leaves the table once it dies.
    //
    // The caller gives the shape to an object, which may be one a marking
    // cycle has already scanned (setPrototypeOf): the prototype is reached
    // only through the shape, so it is shaded here.
    gcShadeEdge(proto);
    for (Shape* root : g_userPrototypeShapes) {
        if (root->prototype.rawBits() == proto.rawBits()) return root;
    }
    Shape* root = Shape::createRoot(rtArena(), proto);
    g_userPrototypeShapes.push_back(root);
    if (gcHoldsYoung(root->prototype)) {
        g_youngUserPrototypeShapes.add(static_cast<uint32_t>(g_userPrototypeShapes.size() - 1));
    }
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
// A minor pass visits only the shapes whose prototype may be young (the
// strong visit is the one a minor needs, and an old prototype needs none).
static void visitUserPrototypeShapes(brass::gc::Tracer& t) {
    auto slotAt = [](uint32_t i) { return prototypeSlot(g_userPrototypeShapes[i]); };
    if (rtRootRemsetPass(t)) {
        g_youngUserPrototypeShapes.visitMinor(t, slotAt);
        return;
    }
    const bool full = t.purpose() == brass::gc::Tracer::Purpose::Full;
    const uint64_t cleared = Value::fromUndefined().rawBits();
    for (Shape* root : g_userPrototypeShapes) {
        auto* slot = prototypeSlot(root);
        if (full) t.visit_weak(slot, cleared);
        else t.visit(slot);
    }
    if (t.purpose() == brass::gc::Tracer::Purpose::Minor) {
        g_youngUserPrototypeShapes.visitMinor(t, slotAt);
    }
}

static void sweepUserPrototypeShapes() {
    if (!rtHeap().collecting_full()) return;
    std::erase_if(g_userPrototypeShapes, [](Shape* root) { return root->prototype.isUndefined(); });
    // The erase shifted the indices the set holds. After a full collection
    // nothing is young and the rebuild finds the set empty; it rebuilds rather
    // than clears so that it says so itself.
    const brass::gc::Heap& gc = rtHeap().gc();
    g_youngUserPrototypeShapes.rebuild(g_userPrototypeShapes.size(), [&gc](size_t i) {
        return gc.holds_young(g_userPrototypeShapes[i]->prototype.rawBits());
    });
}

static thread_local Shape* g_plainObjectShape = nullptr;

Shape* rtPlainObjectShape() {
    if (!g_plainObjectShape) {
        Shape* shape = Shape::createRoot(rtArena(), rtObjectPrototype());
        pushRootShape(shape);
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
    if (shape) pushRootShape(shape);
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


static void eraseSingletonCode(std::unordered_map<SingletonKey, size_t, SingletonKeyHash>& index,
                               const void* code) {
    std::erase_if(index, [code](const auto& kv) { return kv.first.code == code; });
}

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

void rtDropModuleCellsWithin(const void* begin, size_t size) {
    if (!g_threadModules || !begin || size == 0) return;
    ThreadModuleState& s = *g_threadModules;
    const auto lo = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t hi = lo + size;
    auto within = [lo, hi](const void* p) {
        const auto a = reinterpret_cast<uintptr_t>(p);
        return a >= lo && a < hi;
    };
    std::erase_if(s.moduleValueCells, [&](const ModuleSpan<Value>& sp) { return within(sp.cells); });
    std::erase_if(s.moduleRememberedCells,
                  [&](const ModuleSpan<Value>& sp) { return within(sp.cells); });
    std::erase_if(s.moduleFnSlots,
                  [&](const ModuleSpan<FnSingletonSlot>& sp) { return within(sp.cells); });
    std::erase_if(s.globalCacheSpans, [&](const ModuleSpan<Value>& sp) { return within(sp.cells); });
    moduleRemsetSpansDropped(s);
}

void rtDropRealmFunctionSingletons(uint32_t realmSlot) {
    if (!g_threadModules || realmSlot == 0) return;
    ThreadModuleState& s = *g_threadModules;
    const size_t before = s.functionSingletons.size();
    std::erase_if(s.functionSingletons,
                  [realmSlot](const SingletonEntry& e) { return e.realm == realmSlot; });
    if (s.functionSingletons.size() == before) return;
    s.functionSingletonIndex.clear();
    for (size_t i = 0; i < s.functionSingletons.size(); ++i) {
        const SingletonEntry& e = s.functionSingletons[i];
        s.functionSingletonIndex[SingletonKey{reinterpret_cast<const void*>(e.code), e.realm}] = i;
    }
    moduleRemsetSingletonsChanged(s);
}

// Removes the innermost `epoch` from `epochs`, if it is there.
static void eraseLastEpoch(std::vector<uint64_t>& epochs, uint64_t epoch) {
    if (auto it = std::find(epochs.rbegin(), epochs.rend(), epoch); it != epochs.rend()) {
        epochs.erase(std::next(it).base());
    }
}

uint64_t rtBeginModuleEpoch() {
    ThreadModuleState& s = threadModules();
    const uint64_t epoch = ++s.moduleEpochCounter;
    s.activeModuleEpochs.push_back(epoch);
    t_stackModuleEpochs.push_back(epoch);
    return epoch;
}

void rtEndModuleEpoch(uint64_t epoch) {
    if (epoch == 0 || !g_threadModules) return;
    eraseLastEpoch(g_threadModules->activeModuleEpochs, epoch);
    eraseLastEpoch(t_stackModuleEpochs, epoch);
}

uint64_t rtCurrentModuleEpoch() noexcept { return stackModuleEpoch(); }

static void dropModuleSpansAndSingletons(ThreadModuleState& s, uint64_t epoch) {
    for (const auto& span : s.moduleFnSlots) {
        if (span.epoch != epoch) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            if (span.cells[i].code) {
                eraseSingletonCode(s.functionSingletonIndex,
                                   reinterpret_cast<const void*>(span.cells[i].code));
            }
        }
    }
    for (const auto& entry : s.functionSingletons) {
        if (entry.epoch == epoch) {
            eraseSingletonCode(s.functionSingletonIndex, reinterpret_cast<const void*>(entry.code));
        }
    }
    std::erase_if(s.functionSingletons, [epoch, &s](const SingletonEntry& entry) {
        return entry.epoch == epoch ||
               !s.functionSingletonIndex.contains(
                   SingletonKey{reinterpret_cast<const void*>(entry.code), entry.realm});
    });
    for (size_t i = 0; i < s.functionSingletons.size(); ++i) {
        const SingletonEntry& e = s.functionSingletons[i];
        s.functionSingletonIndex[SingletonKey{reinterpret_cast<const void*>(e.code), e.realm}] = i;
    }
    std::erase_if(s.moduleValueCells,
                  [epoch](const ModuleSpan<Value>& sp) { return sp.epoch == epoch; });
    std::erase_if(s.moduleRememberedCells,
                  [epoch](const ModuleSpan<Value>& sp) { return sp.epoch == epoch; });
    std::erase_if(s.moduleFnSlots,
                  [epoch](const ModuleSpan<FnSingletonSlot>& sp) { return sp.epoch == epoch; });
    std::erase_if(s.globalCacheSpans,
                  [epoch](const ModuleSpan<Value>& sp) { return sp.epoch == epoch; });
    moduleRemsetSpansDropped(s);
    moduleRemsetSingletonsChanged(s);
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

static void visitDynamicModuleSpans(ThreadModuleState& s, uint64_t epoch, brass::gc::Tracer& t) {
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
    for (const auto* spans : {&s.moduleValueCells, &s.moduleRememberedCells, &s.globalCacheSpans}) {
        for (const auto& span : *spans) {
            if (span.epoch != epoch) continue;
            for (uint64_t i = 0; i < span.count; ++i) {
                t.visit(reinterpret_cast<uint64_t*>(&span.cells[i]));
            }
        }
    }
}

static void markDynamicModuleSpans(ThreadModuleState& s, DynamicModule* mod, brass::gc::Tracer& t) {
    if (!mod || mod->marked.load(std::memory_order_relaxed)) return;
    if (mod->marked.exchange(true, std::memory_order_acq_rel)) return;
    visitDynamicModuleSpans(s, mod->epoch, t);
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
    if (rtRootRemsetPass(t)) {
        // The shape tables' check rides along here, so that one source of
        // this file runs every check of its tables once a minor.
        if (rtRootRemsetVerifying(t)) {
            const std::string miss = shapeTablesRemsetCheck(t.heap());
            if (!miss.empty()) fatal(miss.c_str());
        }
        moduleTablesVisitMinor(s, t);
        return;
    }
    const bool full = (t.purpose() == brass::gc::Tracer::Purpose::Full) && !s.dynamicModules.empty();
    if (full) {
        // `marked` is cleared after a full collection's sweep
        // (sweepDynamicModules), not here: a marking cycle visits the roots
        // at its start and again at its finish, and a module marked between
        // (through a function the marker reached) must stay marked. A module
        // already marked has its spans visited again: they are roots, written
        // with no barrier, and the finish's visit is what sees those writes.
        for (const auto& mod : s.dynamicModules) {
            if (mod->marked.load(std::memory_order_relaxed)) visitDynamicModuleSpans(s, mod->epoch, t);
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
    for (const auto* spans : {&s.moduleValueCells, &s.moduleRememberedCells, &s.globalCacheSpans}) {
        for (const auto& span : *spans) {
            if (full && isDynamicModuleEpoch(s, span.epoch)) continue;
            for (uint64_t i = 0; i < span.count; ++i) {
                t.visit(reinterpret_cast<uint64_t*>(&span.cells[i]));
            }
        }
    }
    // A minor with the sets off (BRONZE_NO_ROOT_REMSET=1) visited everything
    // above; the sets are still pruned, so they are exact if switched on.
    if (t.purpose() == brass::gc::Tracer::Purpose::Minor) {
        s.youngCells.visitMinor(t);
        s.youngSingletons.visitMinor(
            t, [&s](uint32_t i) { return reinterpret_cast<uint64_t*>(&s.functionSingletons[i].value); });
    }
}

// The shape tables' half of the verification walk (root_remset.h).
static std::string shapeTablesRemsetCheck(const brass::gc::Heap& heap) {
    RootRemsetStats& stats = rtRootRemsetStats();
    struct Table {
        const char* name;
        const std::vector<Shape*>* shapes;
        RootIndexSet* young;
    };
    const Table tables[] = {
        {"the root shapes' prototypes", &g_rootShapes, &g_youngRootShapes},
        {"the user prototype shapes' prototypes", &g_userPrototypeShapes, &g_youngUserPrototypeShapes},
    };
    for (const Table& table : tables) {
        table.young->normalize();
        for (size_t i = 0; i < table.shapes->size(); ++i) {
            const uint64_t bits = (*table.shapes)[i]->prototype.rawBits();
            ++stats.verifiedCells;
            if (heap.holds_young(bits) && !table.young->contains(static_cast<uint32_t>(i))) {
                return rtRootRemsetMissText(table.name, i, bits);
            }
        }
    }
    return {};
}

std::string rtModuleRootsRemsetCheck(const brass::gc::Heap& heap) {
    std::string miss = shapeTablesRemsetCheck(heap);
    if (miss.empty() && g_threadModules) miss = moduleTablesRemsetCheck(*g_threadModules, heap);
    return miss;
}

void rtNoteRootCellWrite(uint64_t* cell) noexcept {
    if (g_threadModules && cell) moduleRemsetNoteCell(*g_threadModules, cell);
}

void rtNoteMethodIcEnvWords(uint64_t* icEntry) noexcept {
    if (!g_threadModules || !icEntry) return;
    moduleRemsetNoteCell(*g_threadModules, icEntry + BRONZE_ABI_METHOD_IC_ENV_WORD);
    moduleRemsetNoteCell(*g_threadModules, icEntry + BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD);
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
    // Unmarked for the next full collection, which marks from its first root
    // visit on (visitModuleAndCacheRoots).
    for (const auto& mod : s.dynamicModules) mod->marked.store(false, std::memory_order_relaxed);
}

static void registerThreadRootSources(Heap& heap) {
    {
        ThreadModuleState& s = threadModules();
        std::lock_guard<std::mutex> lock(g_heapStateMu);
        g_heapToModuleState[&heap.gc()] = &s;
        g_heapStateGeneration.fetch_add(1, std::memory_order_release);
    }
    heap.add_tracer_source(visitRootShapes);
    heap.add_tracer_source(visitUserPrototypeShapes);
    heap.add_tracer_source(visitModuleAndCacheRoots);
    heap.add_post_collection_hook([]() {
        sweepUserPrototypeShapes();
        sweepDynamicModules();
    });
    // Not remembered: the realms' globals, module registries and host globals
    // are written through accessors that hand out the tables themselves
    // (Realm::hostGlobals, moduleRegistry), there are a handful of entries per
    // realm, and the array-method table is forty cells.
    heap.add_root_source([](const Heap::RootVisitor& visit) {
        rtVisitPreRealmHostGlobals(visit);
        rtVisitArrayMethodRoots(visit);
        rtVisitRealmRoots(visit);
    });
}


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
        {reinterpret_cast<Value*>(cells), count, stackModuleEpoch()});
}

// The template cells, which only `bronze_template_object` stores into, so a
// minor visits only the ones it filled with a young object (root_remset.h).
// An image built before this entry existed registers them with
// `bronze_register_value_cells` instead, and they are visited whole.
void bronze_register_template_cells(uint64_t* cells, uint64_t count) {
    if (!cells || count == 0) return;
    ThreadModuleState& s = threadModules();
    s.moduleRememberedCells.push_back(
        {reinterpret_cast<Value*>(cells), count, stackModuleEpoch()});
    moduleRemsetSpanAdded(s, s.moduleRememberedCells.back());
}

void bronze_register_fn_slots(uint64_t* cells, uint64_t count) {
    if (!cells || count == 0) return;
    ThreadModuleState& s = threadModules();
    s.moduleFnSlots.push_back(
        {reinterpret_cast<FnSingletonSlot*>(cells), count, stackModuleEpoch()});
    moduleRemsetFnSlotsAdded(s, s.moduleFnSlots.back());
}

void bronze_register_method_ic_cells(uint64_t* icTable, const uint64_t* siteIndexes,
                                     uint64_t count) {
    if (!icTable || !siteIndexes || count == 0) return;
    ThreadModuleState& s = threadModules();
    constexpr uint64_t kSiteWords = BRONZE_ABI_IC_SITE_SIZE / sizeof(uint64_t);
    for (uint64_t k = 0; k < count; ++k) {
        uint64_t* site = icTable + siteIndexes[k] * kSiteWords;
        // Remembered: only the latches in rt_method_call.cpp store an env
        // word, and each notes the words it wrote (rtNoteMethodIcEnvWords).
        for (uint64_t word : {uint64_t{BRONZE_ABI_METHOD_IC_ENV_WORD},
                              uint64_t{BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD}}) {
            s.moduleRememberedCells.push_back(
                {reinterpret_cast<Value*>(site + word), 1, stackModuleEpoch()});
            moduleRemsetSpanAdded(s, s.moduleRememberedCells.back());
        }
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
    if (auto it = idxMap.find(SingletonKey{reinterpret_cast<const void*>(code), 0});
        it != idxMap.end()) {
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

extern "C" {

uint64_t bronze_function_singleton(bronze_fn_code code, uint32_t arity, uint32_t length,
                                   uint32_t nameKey, uint32_t fnFlags, uint64_t* slotCell) {
    recordHelperCall("bronze_function_singleton");
    ThreadModuleState& s = threadModules();
    Value result = Value::fromUndefined();
    // A native is one of the intrinsics every realm shares; a compiled
    // function is its realm's, one per realm (SingletonEntry::realm).
    const bool native = (fnFlags & BRONZE_ABI_FN_FLAG_NATIVE) != 0;
    const uint32_t realm = native ? 0 : rtCurrentRealmSlot();
    const SingletonKey key{reinterpret_cast<const void*>(code), realm};
    if (auto it = s.functionSingletonIndex.find(key); it != s.functionSingletonIndex.end()) {
        result = s.functionSingletons[it->second].value;
    } else {
        FunctionHeader* fn =
            FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity, fnFlags);
        fn->env_record = Value::fromObject(fn);
        fn->header.flags = HeapKind::Function;
        fn->setRealmSlot(realm);
        rtSetFunctionNameAndLength(fn, nameKey, length);
        result = Value::fromObject(fn);
        uint64_t fnEpoch = 0;
        if ((fnFlags & BRONZE_ABI_FN_FLAG_NATIVE) == 0) {
            if (auto modIt = s.codeToModule.find(reinterpret_cast<const void*>(code));
                modIt != s.codeToModule.end()) {
                fnEpoch = modIt->second->epoch;
            } else {
                fnEpoch = stackModuleEpoch();
            }
        }
        s.functionSingletons.push_back(SingletonEntry{code, result, fnEpoch, realm});
        s.functionSingletonIndex.emplace(key, s.functionSingletons.size() - 1);
        if (gcHoldsYoung(result)) {
            s.youngSingletons.add(static_cast<uint32_t>(s.functionSingletons.size() - 1));
        }
    }
    if (slotCell) {
        auto* slot = reinterpret_cast<FnSingletonSlot*>(slotCell);
        *slot = FnSingletonSlot{code, result};
        moduleRemsetNoteCell(s, reinterpret_cast<uint64_t*>(&slot->value));
    }
    return result.rawBits();
}

}  // extern "C"


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
    if (cacheCell) {
        *cacheCell = resolved.rawBits();
        rtNoteRootCellWrite(cacheCell);
    }
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
            uint64_t spanEpoch = stackModuleEpoch();
            for (const auto* spans : {&s.moduleValueCells, &s.moduleRememberedCells}) {
                for (const auto& vspan : *spans) {
                    if (vspan.cells + vspan.count == valueCells) {
                        spanEpoch = vspan.epoch;
                        break;
                    }
                }
            }
            s.globalCacheSpans.push_back({valueCells, count, spanEpoch});
            moduleRemsetSpanAdded(s, s.globalCacheSpans.back());
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
    if (cacheable && cells && slot < count) {
        valueCells[slot] = resolved;
        moduleRemsetNoteCell(s, cells + slot);
    }
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
