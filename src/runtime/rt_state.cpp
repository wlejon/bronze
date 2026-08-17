// The runtime's process-wide state: the heap, the non-moving arena, the root
// shapes, the property-key INTERN table, and the caches and module spans whose
// entries are heap Values and therefore need root sources. All of it lives in
// this one translation unit so the collector's roots never depend on cross-TU
// static initialization order (rt_state.h).

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// Generated code roots its Dynamic values, so a collection is survivable and
// the reservation does not have to postpone one. Sized so ordinary programs DO
// collect rather than run to exit inside one semispace.
static Heap g_heap(64 * 1024 * 1024);
static NonMovingArena g_arena;

// Root shapes the runtime has created. Shapes are immortal but the prototype
// objects they name are not, so the collector has to forward them; this table
// is what the root source below walks. It lives beside g_arena and g_heap
// because that is where the three lifetimes match — a global registry would
// outlive the per-test arenas unit tests create and hand the collector dangling
// shapes.
static std::vector<Shape*> g_rootShapes;

static const bool g_shapeRootsRegistered = [] {
    g_heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (Shape* root : g_rootShapes) visit(root->prototype);
    });
    return true;
}();

Heap& rtHeap() { return g_heap; }
NonMovingArena& rtArena() { return g_arena; }

Shape* rtNewRootShape(Value proto) {
    Shape* root = Shape::createRoot(g_arena, proto);
    g_rootShapes.push_back(root);
    return root;
}

Shape* rtRootShapeForPrototype(Value proto) {
    // Memoized, and the memo needs no roots of its own: `g_rootShapes` already
    // forwards every root shape's prototype slot, so comparing against that
    // slot compares two CURRENT addresses. A table keyed on a private copy of
    // the prototype would be the Map index's problem all over again — an
    // address recorded before a collection and compared after one.
    //
    // Memoizing at all is what keeps `Object.create(proto)` in a loop from
    // minting a hidden class per object: without it every created object would
    // have a shape no inline cache had ever seen, and each call would leak an
    // arena shape and a root-source entry.
    //
    // The list is its own and not a scan of `g_rootShapes`, because the
    // namespace objects hold root shapes with no prototype ON PURPOSE — so
    // that a site reading `Math.sqrt` does not share a transition tree with
    // `{}` literals — and a scan would hand one of those out.
    static std::vector<Shape*> prototypeShapes;
    for (Shape* root : prototypeShapes) {
        if (root->prototype.rawBits() == proto.rawBits()) return root;
    }
    Shape* root = rtNewRootShape(proto);
    prototypeShapes.push_back(root);
    return root;
}

static Shape* g_plainObjectShape = nullptr;
extern "C" {
uint64_t bronze_plain_shape = 0;
}

Shape* rtPlainObjectShape() {
    if (!g_plainObjectShape) {
        Shape* shape = Shape::createRoot(g_arena, rtObjectPrototype());
        g_rootShapes.push_back(shape);
        g_plainObjectShape = shape;
        bronze_plain_shape = reinterpret_cast<uint64_t>(shape);
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

static std::vector<std::string> g_keyStrings;
// The same keys as immortal arena strings, so a property access allocates
// nothing on the path that reaches one.
static std::vector<StringHeader*> g_keyHeaders;
static std::vector<KeyInfo> g_keyInfos;
// Text -> id, which is what makes the registry an INTERN table rather than an
// array a module fills by index. Two modules that both mention "position" must
// come out holding one id: shapes, inline caches and `Object.keys` all identify
// a property by its key id, so the same string arriving as two ids would give
// one object two indistinguishable properties. Nothing iterates this map, so
// its ordering never reaches output.
static std::unordered_map<std::string, uint32_t> g_keyIndex;
static const std::string g_emptyKey;
static const KeyInfo g_emptyKeyInfo{};

const std::string& rtKeyString(uint32_t index) {
    return index < g_keyStrings.size() ? g_keyStrings[index] : g_emptyKey;
}

StringHeader* rtKeyHeader(uint32_t index) {
    return index < g_keyHeaders.size() ? g_keyHeaders[index] : nullptr;
}

const KeyInfo& rtKeyInfo(uint32_t index) {
    return index < g_keyInfos.size() ? g_keyInfos[index] : g_emptyKeyInfo;
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
static std::vector<std::pair<bronze_fn_code, Value>> g_functionSingletons;
static std::unordered_map<void*, size_t> g_functionSingletonIndex;

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
// A span is never removed. There is no unload — a compiled module's code and
// data live for the process — so an unregister would be a way to hand the
// collector a dangling span and nothing else.
template <typename Cell>
struct ModuleSpan {
    Cell* cells;
    uint64_t count;
};
static std::vector<ModuleSpan<Value>> g_moduleValueCells;
static std::vector<ModuleSpan<FnSingletonSlot>> g_moduleFnSlots;

// Globals an embedding host registered (host_globals.h). A vector of pairs
// rather than a map: registration happens a handful of times at host startup,
// and a lookup is a read `bronze_global_get` reaches only for a name the
// builtin ladder did not answer.
static std::vector<std::pair<std::string, Value>> g_hostGlobals;

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
static const bool g_valueCachesRegistered = [] {
    g_heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (auto& entry : g_functionSingletons) visit(entry.second);
        for (const auto& span : g_moduleFnSlots) {
            for (uint64_t i = 0; i < span.count; ++i) {
                if (span.cells[i].code) visit(span.cells[i].value);
            }
        }
        for (const auto& span : g_moduleValueCells) {
            for (uint64_t i = 0; i < span.count; ++i) visit(span.cells[i]);
        }
        for (auto& entry : g_hostGlobals) visit(entry.second);
        rtVisitArrayMethodRoots(visit);
    });
    return true;
}();

void rtRegisterHostGlobal(const std::string& name, Value value) {
    for (auto& entry : g_hostGlobals) {
        if (entry.first == name) {
            entry.second = value;
            return;
        }
    }
    g_hostGlobals.emplace_back(name, value);
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
    g_moduleValueCells.push_back({reinterpret_cast<Value*>(cells), count});
}

void bronze_register_fn_slots(uint64_t* cells, uint64_t count) {
    if (!cells || count == 0) return;
    g_moduleFnSlots.push_back({reinterpret_cast<FnSingletonSlot*>(cells), count});
}

uint64_t bronze_function_singleton(bronze_fn_code code, uint32_t arity, uint32_t length,
                                   uint32_t nameKey, uint64_t* slotCell) {
    recordHelperCall("bronze_function_singleton");
    // The by-code-pointer map is the authority; it replaced a linear scan
    // that every native builtin ever interned lengthened for every mention of
    // every top-level declaration.
    Value result = Value::fromUndefined();
    if (auto it = g_functionSingletonIndex.find(reinterpret_cast<void*>(code));
        it != g_functionSingletonIndex.end()) {
        result = g_functionSingletons[it->second].second;
    } else {
        FunctionHeader* fn = FunctionHeader::create(g_heap, code, Value::fromUndefined(), arity);
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
// `--host-globals` manifest admitted; anything else becomes `ref.error`, which
// raises the JS ReferenceError. A name in NEITHER the builtin ladder nor the
// host registry is therefore still a drift between lowering's list and this
// one — an internal tripwire, not a program error — and a manifest name the
// host never registered is the same drift with the host on one side of it.
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
    } else if (Value typed = rtTypedArrayConstructor(keyStr); typed.isObject()) {
        out = typed;
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
    Value resolved = Value::fromUndefined();
    if (!rtResolveBuiltinGlobal(keyStr, resolved)) {
        // AFTER every builtin, so a host cannot swap out `Math` under code
        // that was compiled against it — and BEFORE the fatal, because a
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

uint32_t bronze_register_key_string(const char* str) {
    const std::string text = str ? str : "";
    if (auto it = g_keyIndex.find(text); it != g_keyIndex.end()) return it->second;

    const uint32_t index = static_cast<uint32_t>(g_keyStrings.size());
    g_keyStrings.push_back(text);
    g_keyHeaders.push_back(nullptr);
    g_keyInfos.emplace_back();
    g_keyIndex.emplace(text, index);

    StringHeader* tmp = StringHeader::createFromUTF8(g_heap, std::string_view(g_keyStrings[index]));
    g_keyHeaders[index] = StringHeader::internToArena(g_arena, tmp);

    KeyInfo info;
    uint32_t elemIdx = 0;
    if (rtIsIntegerLikeKey(g_keyStrings[index], elemIdx)) {
        info.isElemIndex = true;
        info.elemIndex = elemIdx;
    } else {
        info.isElemIndex = false;
        info.elemIndex = UINT32_MAX;
    }
    info.isLength = (g_keyStrings[index] == "length");
    g_keyInfos[index] = info;
    return index;
}

}  // extern "C"

}  // namespace bronze::runtime
