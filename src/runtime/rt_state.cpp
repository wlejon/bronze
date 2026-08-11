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
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// Generated code roots its Dynamic values (docs/0006), so a collection is
// survivable and the reservation does not have to postpone one. Sized so
// ordinary programs DO collect rather than run to exit inside one semispace.
static Heap g_heap(64 * 1024 * 1024);
static NonMovingArena g_arena;

// Root shapes the runtime has created. Shapes are immortal but the prototype
// objects they name are not, so the collector has to forward them; this table
// is what the root source below walks (docs/0008 decision 1). It lives beside
// g_arena and g_heap because that is where the three lifetimes match — a
// global registry would outlive the per-test arenas unit tests create and
// hand the collector dangling shapes.
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

Shape* rtPlainObjectShape() {
    // Prototype undefined until there is an Object.prototype to point at.
    static Shape* shape = rtNewRootShape(Value::fromUndefined());
    return shape;
}

// ---- ABI pins ---------------------------------------------------------------
//
// Generated code open-codes the boxing constants and the object test of the
// inline property fast path (docs/0010 decision 7), so every constant it uses
// is pinned against the value model here.

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
static_assert(sizeof(Float32ArrayHeader) - sizeof(HeapObjectHeader) >= BRONZE_ABI_OBJ_MIN_PAYLOAD);
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
// otherwise `Foo.prototype.m = ...` would decorate one object and `new Foo()`
// would read another (docs/0008). Keyed on the code pointer, which is 1:1 with
// the declaration; closures never come here, since their identity is
// per-evaluation and they carry an environment.
static std::vector<std::pair<bronze_fn_code, Value>> g_functionSingletons;

// The free identifiers lowering is allowed to resolve (docs/0011 decision 1),
// cached per key index: every mention of `Math` in the source is one lookup,
// including one inside a loop, so a string compare per reference is not a
// thing to leave in a hot path.
static std::vector<Value> g_globalCache;

// Both hold heap Values, so both are root SOURCES rather than fixed slots:
// the objects live in the moving heap and cached raw bits would go stale at
// the first collection.
static const bool g_valueCachesRegistered = [] {
    g_heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (auto& entry : g_functionSingletons) visit(entry.second);
        for (Value& v : g_globalCache) visit(v);
    });
    return true;
}();

extern "C" {

uint64_t bronze_function_singleton(bronze_fn_code code, uint32_t arity) {
    for (const auto& entry : g_functionSingletons) {
        if (entry.first == code) return entry.second.rawBits();
    }
    FunctionHeader* fn = FunctionHeader::create(g_heap, code, Value::fromUndefined(), arity);
    fn->header.flags = 2;
    g_functionSingletons.emplace_back(code, Value::fromObject(fn));
    return g_functionSingletons.back().second.rawBits();
}

// An unknown name never reaches here: lowering diagnoses it at compile time,
// which is why the miss below is an internal tripwire rather than a JS
// ReferenceError.
uint64_t bronze_global_get(uint32_t keyIndex) {
    if (keyIndex < g_globalCache.size() && !g_globalCache[keyIndex].isUndefined()) {
        return g_globalCache[keyIndex].rawBits();
    }
    const std::string& keyStr = rtKeyString(keyIndex);
    Value resolved = Value::fromUndefined();
    if (keyStr == "Math") {
        resolved = rtMathObject();
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
