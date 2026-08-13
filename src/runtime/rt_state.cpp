// The runtime's process-wide state: the heap, the non-moving arena, the root
// shapes, the property-key registry, and the two caches whose entries are
// heap Values and therefore need root sources. All of it lives in this one
// translation unit so the collector's roots never depend on cross-TU static
// initialization order (rt_internal.h).

#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/host_globals.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_internal.h"
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

Shape* rtPlainObjectShape() {
    // The edge that makes `Object.prototype` the value model rather than a
    // builtin: every `{}` literal, every class prototype and every object the
    // runtime builds for a program starts from this root, so naming the
    // intrinsic here is what puts it on all of their chains at once. Nothing on
    // the property path needed changing for it — the walk that already found a
    // class's methods finds these.
    //
    // Built on FIRST USE, and `rtObjectPrototype()` allocates: it must not be
    // reached before the heap is usable, which the lazy static guarantees since
    // the first plain object in a program is the earliest anything can ask.
    static Shape* shape = [] {
        Shape* s = rtNewRootShape(rtObjectPrototype());
        (void)rtFunctionPrototypeObject();
        return s;
    }();
    return shape;
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
static const std::string g_emptyKey;

const std::string& rtKeyString(uint32_t index) {
    return index < g_keyStrings.size() ? g_keyStrings[index] : g_emptyKey;
}

StringHeader* rtKeyHeader(uint32_t index) {
    return index < g_keyHeaders.size() ? g_keyHeaders[index] : nullptr;
}

// ---- Caches with heap Values ------------------------------------------------

// The one function object for a top-level function declaration. A declaration
// is evaluated once, so every mention of its name must yield the SAME object —
// otherwise `Foo.prototype.m =...` would decorate one object and `new Foo()`
// would read another. Keyed on the code pointer, which is 1:1 with the
// declaration; closures never come here, since their identity is per-evaluation
// and they carry an environment.
static std::vector<std::pair<bronze_fn_code, Value>> g_functionSingletons;

// The free identifiers lowering is allowed to resolve, cached per key index:
// every mention of `Math` in the source is one lookup, including one inside a
// loop, so a string compare per reference is not a thing to leave in a hot
// path.
static std::vector<Value> g_globalCache;

// The module scope's environment record. The top level runs exactly once, so
// this scope has exactly one activation and its record is a singleton — which
// is what lets a top-level function declaration reach module-level
// `let`/`const` while staying a direct-call target, instead of being handed the
// record through a calling convention it does not have.
//
// `main` publishes it before any statement runs; the module functions that
// need it load it at entry.
static Value g_moduleEnv = Value::fromUndefined();

// Globals an embedding host registered (host_globals.h). A vector of pairs
// rather than a map: registration happens a handful of times at host startup,
// and a lookup is a read `bronze_global_get` reaches only for a name the
// builtin ladder did not answer.
static std::vector<std::pair<std::string, Value>> g_hostGlobals;

// All four hold heap Values, so all four are root SOURCES rather than fixed
// slots: the objects live in the moving heap and cached raw bits would go stale
// at the first collection. The module environment is the one whose absence here
// would be invisible until a collection ran with a closure alive over it, which
// is exactly what oracle-gc-stress forces at every allocation.
static const bool g_valueCachesRegistered = [] {
    g_heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (auto& entry : g_functionSingletons) visit(entry.second);
        for (Value& v : g_globalCache) visit(v);
        for (auto& entry : g_hostGlobals) visit(entry.second);
        visit(g_moduleEnv);
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

void bronze_module_env_set(uint64_t envBits) { g_moduleEnv = Value(envBits); }

uint64_t bronze_module_env_get() { return g_moduleEnv.rawBits(); }

uint64_t bronze_function_singleton(bronze_fn_code code, uint32_t arity, uint32_t length,
                                   uint32_t nameKey) {
    for (const auto& entry : g_functionSingletons) {
        if (entry.first == code) return entry.second.rawBits();
    }
    FunctionHeader* fn = FunctionHeader::create(g_heap, code, Value::fromUndefined(), arity);
    fn->header.flags = HeapKind::Function;
    rtSetFunctionNameAndLength(fn, nameKey, length);
    g_functionSingletons.emplace_back(code, Value::fromObject(fn));
    return g_functionSingletons.back().second.rawBits();
}

// An unknown name never reaches here. Lowering emits this instruction only for
// a name on its provided-globals list — the closed builtin set, plus whatever a
// `--host-globals` manifest admitted; anything else becomes `ref.error`, which
// raises the JS ReferenceError. A name in NEITHER the builtin ladder nor the
// host registry is therefore still a drift between lowering's list and this
// one — an internal tripwire, not a program error — and a manifest name the
// host never registered is the same drift with the host on one side of it.
uint64_t bronze_global_get(uint32_t keyIndex) {
    if (keyIndex < g_globalCache.size() && !g_globalCache[keyIndex].isUndefined()) {
        return g_globalCache[keyIndex].rawBits();
    }
    const std::string& keyStr = rtKeyString(keyIndex);
    Value resolved = Value::fromUndefined();
    if (keyStr == "Math") {
        resolved = rtMathObject();
    } else if (keyStr == "Object") {
        resolved = rtObjectNamespace();
    } else if (keyStr == "Function") {
        resolved = rtFunctionConstructorObject();
    } else if (keyStr == "JSON") {
        resolved = rtJsonNamespace();
    } else if (keyStr == "Symbol") {
        resolved = rtSymbolFunction();
    } else if (Value regexp = rtRegExpConstructor(keyStr); regexp.isObject()) {
        resolved = regexp;
    } else if (Value collection = rtMapConstructor(keyStr); collection.isObject()) {
        resolved = collection;
    } else if (Value weak = rtWeakCollectionConstructor(keyStr); weak.isObject()) {
        resolved = weak;
    } else if (Value typed = rtTypedArrayConstructor(keyStr); typed.isObject()) {
        resolved = typed;
    } else if (Value dataView = rtDataViewConstructor(keyStr); dataView.isObject()) {
        resolved = dataView;
    } else if (Value global = rtGlobalConstructor(keyStr); global.isObject()) {
        resolved = global;
    } else if (Value ctor = rtErrorConstructor(keyStr); ctor.isObject()) {
        resolved = ctor;
    } else if (Value promise = rtPromiseConstructor(keyStr); promise.isObject()) {
        resolved = promise;
    } else if (Value numeric = rtGlobalNumericFunction(keyStr); numeric.isObject()) {
        resolved = numeric;
    } else if (Value host = Value::fromUndefined(); rtHostGlobalLookup(keyStr, host)) {
        // AFTER every builtin, so a host cannot swap out `Math` under code
        // that was compiled against it — and BEFORE the fatal, because a
        // host-registered name is a legitimate answer. Returned directly
        // rather than through `resolved`, which the cache below would pin:
        // rtRegisterHostGlobal replaces on re-registration, and a cached
        // first answer would keep serving the old value. The registry scan
        // per read is the price, paid only by host-global reads.
        return host.rawBits();
    } else {
        fatal(("internal: no global named " + keyStr).c_str());
    }
    if (keyIndex >= g_globalCache.size()) {
        g_globalCache.resize(keyIndex + 1, Value::fromUndefined());
    }
    g_globalCache[keyIndex] = resolved;
    return resolved.rawBits();
}

void bronze_register_key_string(uint32_t index, const char* str) {
    if (index >= g_keyStrings.size()) {
        g_keyStrings.resize(index + 1);
        g_keyHeaders.resize(index + 1, nullptr);
    }
    g_keyStrings[index] = str ? str : "";
    StringHeader* tmp = StringHeader::createFromUTF8(g_heap, std::string_view(g_keyStrings[index]));
    g_keyHeaders[index] = StringHeader::internToArena(g_arena, tmp);
}

}  // extern "C"

}  // namespace bronze::runtime
