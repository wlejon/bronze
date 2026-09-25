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
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <brass/gc/runtime_gc.hpp>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/iterator.h"
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

// The heap and arena are LEAKED POINTERS behind lazy accessors, not
// thread_local objects, for two reasons. Lazy: a thread that never touches
// the runtime must not pay a 64MB reservation at thread start. Leaked: the
// per-thread tables above and below (weak-ref lists, root shapes, key
// headers) are destroyed at thread exit in an order nothing controls, and a
// heap destructor running among them would be teardown-order roulette — the
// process (or the thread, holding nothing) exits instead, exactly as the
// process-global heap always did.
//
// The registrations that used to ride static initializers (the shape-root
// walk and the value-cache walk below) happen HERE, per thread, because a
// fresh heap needs its root sources before its first collection — and the
// lambdas read the calling thread's thread_local tables, which is the right
// table because a heap only ever collects on its own thread.
static void registerThreadRootSources(Heap& heap);
static void visitBrassThreadRoots(const Heap::RootVisitor& visit);

// Generated code roots its Dynamic values, so a collection is survivable and
// the reservation does not have to postpone one. Sized so ordinary programs DO
// collect rather than run to exit inside one semispace.
Heap& rtHeap() {
    static thread_local Heap* heap = nullptr;
    if (!heap) {
        heap = new Heap(1024 * 1024 * 1024);
        registerThreadRootSources(*heap);
    }
    return *heap;
}

NonMovingArena& rtArena() {
    static thread_local NonMovingArena* arena = nullptr;
    if (!arena) arena = new NonMovingArena();
    return *arena;
}

Shape* rtNewRootShape(Value proto) {
    Shape* root = Shape::createRoot(rtArena(), proto);
    g_rootShapes.push_back(root);
    return root;
}

Shape* rtRootShapeForPrototype(Value proto) {
    // Memoized, without permanently rooting user prototypes. User prototype
    // root shapes live in `g_userPrototypeShapes` and are swept after collections
    // when their prototype object dies in Cheney from-space (`Heap::survivor_of`).
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

static void sweepUserPrototypeShapes() {
    Heap& heap = rtHeap();
    size_t keep = 0;
    for (size_t i = 0; i < g_userPrototypeShapes.size(); ++i) {
        Shape* root = g_userPrototypeShapes[i];
        Value proto = root->prototype;
        if (!proto.isPointer()) {
            g_userPrototypeShapes[keep++] = root;
            continue;
        }
        auto* hdr = reinterpret_cast<HeapObjectHeader*>(proto.payload());
        HeapObjectHeader* live = heap.survivor_of(hdr);
        if (!live) {
            root->prototype = Value::fromUndefined();
            continue;
        }
        root->prototype = Value::fromTagAndPayload(proto.tag(), reinterpret_cast<uintptr_t>(live));
        g_userPrototypeShapes[keep++] = root;
    }
    g_userPrototypeShapes.resize(keep);
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

// ---- Property keys ----------------------------------------------------------

static std::mutex g_keyMutex;
static std::deque<std::string> g_keyStrings;
static std::deque<KeyInfo> g_keyInfos;
static std::unordered_map<std::string, uint32_t> g_keyIndex;
static const std::string g_emptyKey;
static const KeyInfo g_emptyKeyInfo{};

static thread_local std::vector<StringHeader*> g_keyHeaders;

const std::string& rtKeyString(uint32_t index) {
    std::lock_guard<std::mutex> lock(g_keyMutex);
    return index < g_keyStrings.size() ? g_keyStrings[index] : g_emptyKey;
}

StringHeader* rtKeyHeader(uint32_t index) {
    if (index >= g_keyHeaders.size()) {
        g_keyHeaders.resize(index + 1, nullptr);
    }
    StringHeader* hdr = g_keyHeaders[index];
    if (!hdr) {
        bool valid = false;
        std::string str;
        {
            std::lock_guard<std::mutex> lock(g_keyMutex);
            if (index < g_keyStrings.size()) {
                str = g_keyStrings[index];
                valid = true;
            }
        }
        if (valid) {
            hdr = StringHeader::createFromUTF8InArena(rtArena(), std::string_view(str));
            g_keyHeaders[index] = hdr;
        }
    }
    return hdr;
}

static thread_local std::vector<const KeyInfo*> g_threadKeyInfos;

const KeyInfo& rtKeyInfo(uint32_t index) {
    if (index < g_threadKeyInfos.size()) {
        const KeyInfo* cached = g_threadKeyInfos[index];
        if (cached != nullptr) return *cached;
    }
    std::lock_guard<std::mutex> lock(g_keyMutex);
    if (index >= g_keyInfos.size()) return g_emptyKeyInfo;
    const KeyInfo* ptr = &g_keyInfos[index];
    if (index >= g_threadKeyInfos.size()) {
        g_threadKeyInfos.resize(index + 1, nullptr);
    }
    g_threadKeyInfos[index] = ptr;
    return *ptr;
}

// ---- Caches with heap Values ------------------------------------------------

// The one function object for a top-level function declaration. A declaration
// is evaluated once, so every mention of its name must yield the SAME object —
// otherwise `Foo.prototype.m =...` would decorate one object and `new Foo()`
// would read another. Keyed on the code pointer, which is 1:1 with the
// declaration; closures never come here, since their identity is per-evaluation
// and they carry an environment. The map is an index into the vector rather
// than holding Values itself so the ROOT SOURCE below stays one flat walk;
// nothing iterates the map, so its ordering never reaches output.
static thread_local std::vector<std::pair<bronze_fn_code, Value>> g_functionSingletons;
static thread_local std::unordered_map<void*, size_t> g_functionSingletonIndex;

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

// The global cache and the fn-singleton slots are arrays in each MODULE's own
// data, and a module hands the runtime its two spans at init. The runtime holds
// only the spans, because the only thing it needs from them is to trace the
// Values they hold: the collector moves objects, so a cell holding pre-move
// bits after a collection is the whole failure these roots exist to prevent.
//
// The epoch is what makes a span removable: a host brackets a module's entry
// with rtBeginModuleEpoch, every span registered inside the bracket carries
// that epoch, and rtDropModuleEpoch takes them back out. Epoch 0 marks a span
// registered outside any bracket — a linked program's — and those are
// permanent. Removal is safe ONLY because the unload contract (embed.h) keeps
// the module's image mapped: a dropped span stops being a ROOT, it does not
// start being dangling memory.
template <typename Cell>
struct ModuleSpan {
    Cell* cells;
    uint64_t count;
    uint64_t epoch;
};
static thread_local std::vector<ModuleSpan<Value>> g_moduleValueCells;
static thread_local std::vector<ModuleSpan<FnSingletonSlot>> g_moduleFnSlots;

// The provided-global CACHE spans: one per module, registered by the module's
// first cache miss (bronze_global_get_cached below) rather than at module init,
// which is why they are their own list and not entries in g_moduleValueCells —
// the runtime has to be able to put the hole back into every one of them when
// a host re-registers a global, and a walk of the value-cell spans would
// overwrite template objects and environment cells it has no business
// touching. Traced by the same root source; dropped by the same epoch.
static thread_local std::vector<ModuleSpan<Value>> g_globalCacheSpans;

// Every cached answer, in every module on this thread, becomes the hole
// again. The next read of each name re-resolves through the ladder and
// refills; a cell that was never filled is unchanged. Cheap because the
// caller is a host REGISTERING a global — a handful of times at startup and
// on a realm swap — and never a read.
void rtInvalidateGlobalCaches() {
    for (const auto& span : g_globalCacheSpans) {
        for (uint64_t i = 0; i < span.count; ++i) span.cells[i] = Value::fromHole();
    }
}

static thread_local uint64_t g_moduleEpochCounter = 0;
static thread_local uint64_t g_currentModuleEpoch = 0;

uint64_t rtBeginModuleEpoch() {
    g_currentModuleEpoch = ++g_moduleEpochCounter;
    return g_currentModuleEpoch;
}

void rtEndModuleEpoch(uint64_t epoch) {
    if (epoch != 0 && g_currentModuleEpoch == epoch) g_currentModuleEpoch = 0;
}

void rtDropModuleEpoch(uint64_t epoch) {
    if (epoch == 0) return;
    // The function singletons this module interned die with it. Their code
    // pointers are 1:1 with declarations in the module's image, and every
    // intern a module performs fills a cell in its own slot span — so the
    // spans being dropped name exactly the by-code-pointer map entries whose
    // last mentioner is going away. The runtime's own interning (null slot
    // cell) lands in no span and is untouched.
    for (const auto& span : g_moduleFnSlots) {
        if (span.epoch != epoch) continue;
        for (uint64_t i = 0; i < span.count; ++i) {
            if (span.cells[i].code) {
                g_functionSingletonIndex.erase(reinterpret_cast<void*>(span.cells[i].code));
            }
        }
    }
    std::erase_if(g_functionSingletons, [](const std::pair<bronze_fn_code, Value>& entry) {
        return !g_functionSingletonIndex.contains(reinterpret_cast<void*>(entry.first));
    });
    // The erase shifted vector positions; the map's indices must follow.
    for (size_t i = 0; i < g_functionSingletons.size(); ++i) {
        g_functionSingletonIndex[reinterpret_cast<void*>(g_functionSingletons[i].first)] = i;
    }
    std::erase_if(g_moduleValueCells,
                  [epoch](const ModuleSpan<Value>& s) { return s.epoch == epoch; });
    std::erase_if(g_moduleFnSlots,
                  [epoch](const ModuleSpan<FnSingletonSlot>& s) { return s.epoch == epoch; });
    std::erase_if(g_globalCacheSpans,
                  [epoch](const ModuleSpan<Value>& s) { return s.epoch == epoch; });
    if (g_currentModuleEpoch == epoch) g_currentModuleEpoch = 0;
}

// Globals an embedding host registered (host_globals.h). A vector of pairs
// rather than a map: registration happens a handful of times at host startup,
// and a lookup is a read `bronze_global_get` reaches only for a name the
// builtin ladder did not answer.
static thread_local std::vector<std::pair<std::string, Value>> g_hostGlobals;

// All of these hold heap Values, so all of them are root SOURCES rather than
// fixed slots: the objects live in the moving heap and cached raw bits would go
// stale at the first collection.
//
// The two module-span walks are what make a compiled module's own tables real
// roots — its global cache, its module-environment cell, and its
// function-singleton slots. Their absence would be invisible until a collection
// ran with a closure alive over one, which is exactly what oracle-gc-stress
// forces at every allocation. A fn slot whose code word is null was never
// filled, and its value word is whatever the module's .bss started as rather
// than a Value — so the code word, not the value, is what says an entry is
// worth tracing.
// Both root sources for a fresh thread's heap, registered from rtHeap()'s
// first-use path. Each lambda reads thread_local tables, and reads the RIGHT
// thread's: a heap only ever collects on the thread that owns it.
static void registerThreadRootSources(Heap& heap) {
    heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (Shape* root : g_rootShapes) visit(root->prototype);
    });
    heap.add_post_collection_hook([]() {
        sweepUserPrototypeShapes();
    });
    heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (auto& entry : g_functionSingletons) visit(entry.second);
        for (const auto& span : g_moduleFnSlots) {
            for (uint64_t i = 0; i < span.count; ++i) {
                if (span.cells[i].code) visit(span.cells[i].value);
            }
        }
        for (const auto& span : g_moduleValueCells) {
            for (uint64_t i = 0; i < span.count; ++i) visit(span.cells[i]);
        }
        for (const auto& span : g_globalCacheSpans) {
            for (uint64_t i = 0; i < span.count; ++i) visit(span.cells[i]);
        }
        for (auto& entry : g_hostGlobals) visit(entry.second);
        rtVisitArrayMethodRoots(visit);
        rtVisitRealmRoots(visit);
    });
    heap.add_root_source(visitBrassThreadRoots);
}

// Every gcref slot brass knows on this thread: its native-frame scopes and
// ThreadRootsScopes (the latter carrying the frames of interpreters a nested
// bronze program hid, brass_tiered_engine.cpp), and the innermost running
// Interpreter and FastInterpreter. Bronze's own code roots its values in
// bronze GC frames and never allocates from a brass heap (brass's heaps are
// configured to forbid allocation, brass_symbol_registration.cpp), so in a
// bronze program this is
// normally empty; it is here so that a gcref held only by a brass frame is
// still a root of the one heap in the process. The collection starts from
// the runtime, not from a generated frame, so no frame pointer is passed:
// generated frames are walked by bronze's own frame chain instead.
//
// brass's contract for a slot is "a gcref, or a tagged value whose low 48
// bits are one". A slot carrying a bronze pointer Value is visited as that
// Value. Any other slot is its low 48 bits as a heap address under whatever
// upper bits it carries (none for a raw gcref, brass's own tag for one of
// its boxed values): forwarded as an object reference and written back
// under the same upper bits.
static void visitBrassThreadRoots(const Heap::RootVisitor& visit) {
    static thread_local std::vector<uintptr_t*> slots;
    slots.clear();
    brass::brass_enumerate_thread_roots(0, 0, slots);
    // A slot may be reported more than once (an interpreter's frames through
    // its ThreadRootsScope and as the innermost one); visit each once.
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    constexpr uint64_t kLow48 = 0x0000FFFFFFFFFFFFULL;
    for (uintptr_t* slot : slots) {
        const uint64_t bits = static_cast<uint64_t>(*slot);
        Value asBronze = Value::fromRawBits(bits);
        if (asBronze.isPointer()) {
            visit(asBronze);
            *slot = static_cast<uintptr_t>(asBronze.rawBits());
            continue;
        }
        const uint64_t address = bits & kLow48;
        if (address == 0) continue;
        Value ref = Value::fromObject(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)));
        visit(ref);
        *slot = static_cast<uintptr_t>((bits & ~kLow48) | ref.payload());
    }
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
            out = entry.second;
            return true;
        }
    }
    return false;
}

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
    g_moduleValueCells.push_back(
        {reinterpret_cast<Value*>(cells), count, g_currentModuleEpoch});
}

void bronze_register_fn_slots(uint64_t* cells, uint64_t count) {
    if (!cells || count == 0) return;
    g_moduleFnSlots.push_back(
        {reinterpret_cast<FnSingletonSlot*>(cells), count, g_currentModuleEpoch});
}

// The env words of the module's method-call sites, as ordinary value cells —
// one single-cell span each, because the words sit one per site at the IC
// table's stride and the span walk wants contiguous cells. TWO cells per site
// since the site grew a second way: word BRONZE_ABI_METHOD_IC_ENV_WORD is way
// 0's env argument and word BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD is way 1's
// (the site contract in bronze_abi.h). A module whose sites never fill way 1
// leaves that word 0 — a double, which the forwarding walk ignores — so
// registering it unconditionally costs one dead visit per site and no
// correctness anywhere. The count is the module's method-site count
// (hundreds, not millions), paid once at init; the per-collection cost is one
// visit per cell, the same bill the global cache already pays.
void bronze_register_method_ic_cells(uint64_t* icTable, const uint64_t* siteIndexes,
                                     uint64_t count) {
    if (!icTable || !siteIndexes || count == 0) return;
    constexpr uint64_t kSiteWords = BRONZE_ABI_IC_SITE_SIZE / sizeof(uint64_t);
    for (uint64_t k = 0; k < count; ++k) {
        uint64_t* site = icTable + siteIndexes[k] * kSiteWords;
        g_moduleValueCells.push_back(
            {reinterpret_cast<Value*>(site + BRONZE_ABI_METHOD_IC_ENV_WORD), 1,
             g_currentModuleEpoch});
        g_moduleValueCells.push_back(
            {reinterpret_cast<Value*>(site + BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD), 1,
             g_currentModuleEpoch});
    }
}

}  // extern "C"

// Default ON: the seam exists to turn the mechanism OFF for an A/B, and a
// thread that never ran heap.cpp's env scan (there is none, but the default
// should not depend on that) behaves like the shipped configuration.
static thread_local bool g_envMethodIcEnabled = true;

void rtSetEnvMethodIcEnabled(bool enabled) { g_envMethodIcEnabled = enabled; }
bool rtEnvMethodIcEnabled() noexcept { return g_envMethodIcEnabled; }

// Default ON for the reason the seam above is: the flag exists to turn the
// exotic-receiver latch OFF for an A/B, and a thread that never ran heap.cpp's
// env scan should behave like the shipped configuration.
static thread_local bool g_exoticMethodIcEnabled = true;

void rtSetExoticMethodIcEnabled(bool enabled) { g_exoticMethodIcEnabled = enabled; }
bool rtExoticMethodIcEnabled() noexcept { return g_exoticMethodIcEnabled; }

static thread_local bool g_polyMethodIcEnabled = true;

void rtSetPolyMethodIcEnabled(bool enabled) { g_polyMethodIcEnabled = enabled; }
bool rtPolyMethodIcEnabled() noexcept { return g_polyMethodIcEnabled; }

// ---- the interned-native table, as the memo in front of it sees it --------
//
// `runtime/native_fn_memo.cpp` keeps a direct-mapped (code -> INDEX) table so
// a repeat interning costs a hash and two loads instead of an unordered_map
// probe and a cross-module call. It stores an index and never a Value, which
// is the whole reason these two functions exist rather than the memo reaching
// into the vector: the index is meaningless without the vector's own
// bounds-and-identity check, and that check is also what makes the memo
// self-healing across a module UNLOAD, which erases entries and renumbers
// everything after them.

uint32_t rtFunctionSingletonIndexOf(bronze_fn_code code) {
    if (auto it = g_functionSingletonIndex.find(reinterpret_cast<void*>(code));
        it != g_functionSingletonIndex.end()) {
        return static_cast<uint32_t>(it->second);
    }
    return UINT32_MAX;
}

Value rtFunctionSingletonAt(uint32_t index, bronze_fn_code expect) {
    if (index >= g_functionSingletons.size()) return Value::fromUndefined();
    const auto& entry = g_functionSingletons[index];
    if (entry.first != expect) return Value::fromUndefined();
    return entry.second;
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
    // The by-code-pointer map is the authority; it replaced a linear scan
    // that every native builtin ever interned lengthened for every mention of
    // every top-level declaration.
    Value result = Value::fromUndefined();
    if (auto it = g_functionSingletonIndex.find(reinterpret_cast<void*>(code));
        it != g_functionSingletonIndex.end()) {
        result = g_functionSingletons[it->second].second;
    } else {
        FunctionHeader* fn =
            FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity, fnFlags);
        fn->env_record = Value::fromObject(fn);
        fn->header.flags = HeapKind::Function;
        rtSetFunctionNameAndLength(fn, nameKey, length);
        result = Value::fromObject(fn);
        g_functionSingletons.emplace_back(code, result);
        g_functionSingletonIndex.emplace(reinterpret_cast<void*>(code),
                                         g_functionSingletons.size() - 1);
    }
    // Fill the calling module's slot so the NEXT mention at this slot needs no
    // call at all. The runtime's own native interning passes null: it has no
    // module, and the by-code-pointer map above already answered.
    if (slotCell) {
        *reinterpret_cast<FnSingletonSlot*>(slotCell) = FnSingletonSlot{code, result};
    }
    return result.rawBits();
}

// An unknown name never reaches here. Lowering emits this instruction only for
// a name on its provided-globals list — the closed builtin set, plus whatever a
// `--host-globals` manifest admitted; anything else becomes `name.resolve`,
// which asks the global object and raises the JS ReferenceError on a miss. A
// name in NEITHER the builtin ladder nor the host registry is therefore still a
// drift between lowering's list and this one — an internal tripwire, not a
// program error — and a manifest name the host never registered is the same
// drift with the host on one side of it.
}  // extern "C"

// The builtin half of global resolution, name in and value out, with no
// cache: `bronze_global_get` fills the calling module's cell, and `rtGlobalThisObject`
// walks this same ladder to give the global object real properties — two
// callers, one list, so `Math` and `globalThis.Math` cannot drift.
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
        // The two module-registry intrinsics the linker's generated source
        // calls (module_registry.h). On the ladder rather than in the host
        // registry because they are the compiler's own, not a host's — and
        // last, so no name a program can reasonably write is shadowed by one.
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
    // Host-registered globals take precedence for platform/host APIs (e.g. `performance`,
    // where a host provides its own clock bound to the engine's virtual / rAF frame seam
    // rather than the standalone runtime's default steady_clock namespace). Returned
    // directly rather than through `resolved`, which the cache below would pin.
    if (keyStr == "performance") {
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            return host.rawBits();
        }
    }
    Value resolved = Value::fromUndefined();
    if (!rtResolveBuiltinGlobal(keyStr, resolved)) {
        // AFTER every language builtin, so a host cannot swap out `Math` under
        // code that was compiled against it — and BEFORE the fatal, because a
        // host-registered name is a legitimate answer. Returned directly
        // rather than through `resolved`, which the cache below would pin:
        // rtRegisterHostGlobal replaces on re-registration, and a cached
        // first answer would keep serving the old value. The registry scan
        // per read is the price, paid only by host-global reads.
        if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
            return host.rawBits();
        }
        // A property of the global object IS a global binding (9.1.1.4.1
        // resolves an unqualified name against the global environment, whose
        // object record is `globalThis`). A program that assigned
        // `globalThis.navigator = {...}` created the global the next free
        // `navigator` reads — so the object is consulted before the fatal,
        // and never cached, because the program can assign again.
        if (Value fromGlobalObject = Value::fromUndefined();
            rtGlobalThisOwnLookup(keyStr, fromGlobalObject)) {
            return fromGlobalObject.rawBits();
        }
        fatal(("internal: no global named " + keyStr).c_str());
    }
    // Only a BUILTIN reaches here, and only a builtin is ever written back: the
    // two fallthroughs above returned directly, which is what keeps their
    // scan-per-read semantics. The cell belongs to the calling module; the
    // runtime's own callers pass none.
    if (cacheCell) *cacheCell = resolved.rawBits();
    return resolved.rawBits();
}

uint64_t bronze_global_get_cached(uint32_t keyIndex, uint64_t* cells, uint64_t count,
                                  uint32_t slot) {
    recordPropCall("bronze_global_get_cached", keyIndex, nullptr);
    // The span, registered on first contact with this module's array. A
    // linear probe: a thread holds a handful of modules, and this runs once
    // per (module, name) — the fast path in generated code never gets here
    // again for a name that filled.
    auto* valueCells = reinterpret_cast<Value*>(cells);
    if (cells && count != 0 && slot < count) {
        bool known = false;
        for (const auto& span : g_globalCacheSpans) {
            if (span.cells == valueCells) {
                known = true;
                break;
            }
        }
        if (!known) g_globalCacheSpans.push_back({valueCells, count, g_currentModuleEpoch});
    }
    const std::string& keyStr = rtKeyString(keyIndex);
    // The same ladder as bronze_global_get, in the same order — `performance`
    // is the one name a host may shadow, every other builtin wins, then the
    // host registry, then the global object's own properties. The difference
    // is WHICH answers fill the cell: builtins and host globals both, because
    // both are stable until the host says otherwise (and rtRegisterHostGlobal
    // says so by holing every span). A globalThis property is the program's
    // to reassign at any moment and stays uncached.
    Value resolved = Value::fromUndefined();
    bool cacheable = false;
    if (keyStr == "globalThis") {
        resolved = rtGlobalThisObject();
        cacheable = false;
    } else if (keyStr == "performance" && rtHostGlobalLookup(keyStr, resolved)) {
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

uint32_t bronze_register_key_string_len(const char* str, size_t len) {
    const std::string text(str ? str : "", str ? len : 0);
    std::lock_guard<std::mutex> lock(g_keyMutex);
    if (auto it = g_keyIndex.find(text); it != g_keyIndex.end()) return it->second;

    const uint32_t index = static_cast<uint32_t>(g_keyStrings.size());
    g_keyStrings.push_back(text);

    KeyInfo info;
    uint32_t elemIdx = 0;
    if (rtIsIntegerLikeKey(text, elemIdx)) {
        info.isElemIndex = true;
        info.elemIndex = elemIdx;
    } else {
        info.isElemIndex = false;
        info.elemIndex = UINT32_MAX;
    }
    info.isLength = (text == "length");
    g_keyInfos.push_back(info);
    g_keyIndex.emplace(text, index);
    return index;
}

uint32_t bronze_register_key_string(const char* str) {
    return bronze_register_key_string_len(str, str ? std::strlen(str) : 0);
}

}  // extern "C"

}  // namespace bronze::runtime
