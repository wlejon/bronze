#include "runtime/rt_helpers.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "runtime/array.h"
#include "runtime/env.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/number_format.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// Generated code roots its Dynamic values now (docs/0006), so a collection
// is survivable and the reservation no longer has to postpone one. Sized so
// ordinary programs DO collect: the 512MB it used to reserve meant nothing
// short of a huge run ever exercised the collector outside gc-stress.
static Heap g_heap(64 * 1024 * 1024);
static NonMovingArena g_arena;
static std::vector<std::string> g_keyStrings;
// The same keys as immortal arena strings: property paths use these
// directly so a property access allocates nothing.
static std::vector<StringHeader*> g_keyHeaders;
static std::vector<InlineCache> g_inlineCaches;
static const std::string g_emptyKey;

// Root shapes the runtime has created. Shapes are immortal but the
// prototype objects they name are not, so the collector has to forward
// them; this table is what the root source below walks (docs/0008
// decision 1). It lives here, beside g_arena and g_heap, because that is
// where the three lifetimes match — a global registry would outlive the
// per-test arenas that unit tests create and hand the collector dangling
// shapes.
static std::vector<Shape*> g_rootShapes;

static const bool g_shapeRootsRegistered = [] {
    g_heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (Shape* root : g_rootShapes) visit(root->prototype);
    });
    return true;
}();

static Shape* newRootShape(Value proto) {
    Shape* root = Shape::createRoot(g_arena, proto);
    g_rootShapes.push_back(root);
    return root;
}

static_assert(Value::fromUndefined().rawBits() == BRONZE_ABI_UNDEFINED_BITS,
              "BRONZE_ABI_UNDEFINED_BITS in bronze_abi.h has drifted from the value model");
static_assert(Value::fromNull().rawBits() == BRONZE_ABI_NULL_BITS,
              "BRONZE_ABI_NULL_BITS in bronze_abi.h has drifted from the value model");

static uint64_t bronze_builtin_string_char_code_at(uint64_t envBits, uint64_t thisBits, uint32_t argc,
                                                   const uint64_t* argvBits) {
    (void)envBits;  // builtins capture nothing
    Value thisArg(thisBits);
    if (!thisArg.isString()) return Value::fromDouble(0.0).rawBits();
    StringHeader* str = thisArg.asString<StringHeader>();
    uint32_t idx = 0;
    if (argc > 0) {
        Value arg0(argvBits[0]);
        if (arg0.isNumber()) {
            idx = static_cast<uint32_t>(arg0.asNumber());
        } else if (arg0.isInt32()) {
            idx = static_cast<uint32_t>(arg0.payload());
        }
    }
    return Value::fromDouble(str->charCodeAt(idx)).rawBits();
}

// Lazily created and cached forever, so it needs a root that outlives every
// frame — a bare pointer here went stale on the first collection after a
// string ever saw `.charCodeAt`.
static Value g_charCodeAtFn = Value::fromUndefined();

static Value valueToString(Value v) {
    if (v.isString()) return v;
    if (v.isNumber()) {
        char buf[64];
        size_t len = formatJsNumber(v.asNumber(), buf);
        StringHeader* sh = StringHeader::createFromUTF8(g_heap, std::string_view(buf, len));
        return Value::fromString(sh);
    } else if (v.isBool()) {
        StringHeader* sh = StringHeader::createFromUTF8(g_heap, v.asBool() ? "true" : "false");
        return Value::fromString(sh);
    } else if (v.isNull()) {
        return Value::fromString(StringHeader::createFromUTF8(g_heap, "null"));
    } else if (v.isUndefined()) {
        return Value::fromString(StringHeader::createFromUTF8(g_heap, "undefined"));
    }
    fatal("ToString on an object is unsupported");
}

// A canonical array index: the decimal form must round-trip, so "0" and
// "42" qualify while "01", "1.0", "-1" and " 1" are ordinary string keys
// (docs/0009 decision 1).
static bool isIntegerLikeKey(std::string_view key, uint32_t& out) {
    if (key.empty() || key.size() > 10) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    uint64_t v = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > 4294967294ull) return false;  // 2^32-2, the last array index
    out = static_cast<uint32_t>(v);
    return true;
}

static std::string_view latin1View(const StringHeader* s) {
    return std::string_view(s->latin1Data(), s->getLength());
}

extern "C" {

uint64_t bronze_box_f64(double v) {
    return Value::fromDouble(v).rawBits();
}

uint64_t bronze_box_i32(int32_t v) {
    return Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Int32), static_cast<uint32_t>(v)).rawBits();
}

uint64_t bronze_box_bool(bool v) {
    return Value::fromBool(v).rawBits();
}

uint64_t bronze_box_str(const char* s) {
    if (!s) return Value::fromUndefined().rawBits();
    StringHeader* sh = StringHeader::createFromUTF8(g_heap, std::string_view(s));
    return Value::fromString(sh).rawBits();
}

uint64_t bronze_box_str_key(uint32_t keyIndex) {
    std::string keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : "";
    return bronze_box_str(keyStr.c_str());
}

double bronze_unbox_f64(uint64_t bits) {
    // ToNumber for the primitives it is defined on; anything needing string
    // parsing or ToPrimitive is a named hard error, never a silent 0.
    Value v(bits);
    if (v.isNumber()) return v.asNumber();
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    if (v.isNull()) return 0.0;
    if (v.isUndefined()) return std::numeric_limits<double>::quiet_NaN();
    fatal("ToNumber on a string or object is unsupported");
}

int32_t bronze_unbox_i32(uint64_t bits) {
    Value v(bits);
    if (v.isInt32()) return static_cast<int32_t>(v.payload());
    if (v.isNumber()) return static_cast<int32_t>(v.asNumber());
    if (v.isBool()) return v.asBool() ? 1 : 0;
    return 0;
}

bool bronze_truthy(uint64_t bits) {
    Value v(bits);
    if (v.isUndefined() || v.isNull() || v.isHole()) return false;
    if (v.isBool()) return v.asBool();
    if (v.isNumber()) {
        double d = v.asNumber();
        return (d != 0.0) && !std::isnan(d);
    }
    if (v.isInt32()) {
        return static_cast<int32_t>(v.payload()) != 0;
    }
    if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        return str && (str->getLength() > 0);
    }
    return true;
}

bool bronze_is_nullish(uint64_t bits) {
    Value v(bits);
    return v.isNull() || v.isUndefined() || v.isHole();
}

bool bronze_strict_eq(uint64_t aBits, uint64_t bBits) {
    Value a(aBits);
    Value b(bBits);
    if (a.isNumber() && b.isNumber()) {
        return a.asNumber() == b.asNumber();  // NaN !== NaN, +0 === -0
    }
    if (a.isString() && b.isString()) {
        return a.asString<StringHeader>()->equals(*b.asString<StringHeader>());
    }
    // Same tag + same payload: bools, null, undefined, object identity.
    // Different tags can never be strictly equal.
    return aBits == bBits;
}

bool bronze_unbox_bool(uint64_t bits) {
    return bronze_truthy(bits);
}

// One root shape shared by every plain `{}` literal (prototype undefined
// until there is an Object.prototype to point at). Per-literal root shapes
// would give two identical literals unrelated hidden classes, so any site
// seeing both would miss its IC every time — docs/0008 decision 1.
static Shape* plainObjectShape() {
    static Shape* shape = newRootShape(Value::fromUndefined());
    return shape;
}

uint64_t bronze_create_object() {
    ObjectHeader* obj = ObjectHeader::create(g_heap, g_arena, plainObjectShape());
    obj->header.flags = 0;
    return Value::fromObject(obj).rawBits();
}

uint64_t bronze_create_array(uint32_t length) {
    uint32_t cap = (length < 4) ? 4 : length;
    ArrayHeader* arr = ArrayHeader::create(g_heap, cap);
    arr->header.flags = 1;
    arr->length = length;
    return Value::fromObject(arr).rawBits();
}

uint64_t bronze_create_function(bronze_fn_code code, uint32_t arity, uint64_t envBits) {
    Rooted<Value> env{Value(envBits)};
    FunctionHeader* fn = FunctionHeader::create(g_heap, code, Value::fromUndefined(), arity);
    // Read the environment through the root only AFTER allocating: the
    // allocation above can collect, and a by-value copy taken before it
    // would point into dead from-space.
    fn->env_record = env.get();
    fn->header.flags = 2;
    return Value::fromObject(fn).rawBits();
}

// A function's `.prototype`, created on first demand — as a constructor's
// instance prototype or as the target of `Foo.prototype.m = ...`, which
// must be the same object, so both go through here (docs/0008 decision 4).
// Allocates, so it takes and returns through a root and the caller must
// re-derive its own pointer afterwards.
static void ensureFunctionPrototype(Rooted<Value>& fnVal) {
    FunctionHeader* fn = fnVal.get().asObject<FunctionHeader>();
    if (fn->prototype.isObject() && fn->instance_shape) {
        return;
    }
    ObjectHeader* proto = ObjectHeader::create(g_heap, g_arena, plainObjectShape());
    proto->header.flags = 0;

    fn = fnVal.get().asObject<FunctionHeader>();  // create() may have moved it
    fn->prototype = Value::fromObject(proto);
    fn->instance_shape = newRootShape(fn->prototype);
}

uint64_t bronze_construct(uint64_t fnBits, uint32_t argc, const uint64_t* argvBits) {
    Value fnVal(fnBits);
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != 2) {
        fatal("new on a value that is not a function");
    }

    Rooted<Value> fnRoot{fnVal};
    ensureFunctionPrototype(fnRoot);

    FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
    ObjectHeader* instance = ObjectHeader::create(g_heap, g_arena, fn->instance_shape);
    instance->header.flags = 0;

    Rooted<Value> self{Value::fromObject(instance)};
    // argv is the caller's rooted frame slots (docs/0006), so the argument
    // values stay live across the callee's allocations without a copy.
    fn = fnRoot.get().asObject<FunctionHeader>();
    Value result = fn->call(self.get(), argc,
                            const_cast<Value*>(reinterpret_cast<const Value*>(argvBits)));

    // JS: a constructor returning an object replaces the instance; any
    // other return value (including undefined) is ignored.
    if (result.isObject()) {
        return result.rawBits();
    }
    return self.get().rawBits();
}

// The one function object for a top-level function declaration. A
// declaration is evaluated once, so every mention of its name must yield
// the SAME object — otherwise `Foo.prototype.m = ...` would decorate one
// object and `new Foo()` would read another (docs/0008). Keyed on the code
// pointer, which is 1:1 with the declaration; closures never come here,
// since their identity is per-evaluation and they carry an environment.
static std::vector<std::pair<bronze_fn_code, Value>>& functionSingletons() {
    static std::vector<std::pair<bronze_fn_code, Value>> table;
    return table;
}

static const bool g_functionSingletonsRooted = [] {
    g_heap.add_root_source([](const Heap::RootVisitor& visit) {
        for (auto& entry : functionSingletons()) visit(entry.second);
    });
    return true;
}();

uint64_t bronze_function_singleton(bronze_fn_code code, uint32_t arity) {
    auto& table = functionSingletons();
    for (const auto& entry : table) {
        if (entry.first == code) return entry.second.rawBits();
    }
    FunctionHeader* fn = FunctionHeader::create(g_heap, code, Value::fromUndefined(), arity);
    fn->header.flags = 2;
    table.emplace_back(code, Value::fromObject(fn));
    return table.back().second.rawBits();
}

uint64_t bronze_object_keys(uint64_t objBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("Object.keys on a value that is not an object");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // An array's own keys are its indices, already in ascending order.
    if (hdr->flags == 1) {
        Rooted<Value> src{objVal};
        uint32_t length = reinterpret_cast<ArrayHeader*>(hdr)->length;
        Rooted<Value> out{Value::fromObject(ArrayHeader::create(g_heap, length ? length : 4))};
        out.get().asObject<ArrayHeader>()->header.flags = 1;
        for (uint32_t i = 0; i < length; ++i) {
            char buf[16];
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), i);
            Rooted<Value> key{Value::fromString(
                StringHeader::createFromUTF8(g_heap, std::string_view(buf, end - buf)))};
            out.get().asObject<ArrayHeader>()->setElem(g_heap, i, key);
        }
        return out.get().rawBits();
    }
    if (hdr->flags != 0) {
        fatal("Object.keys is only supported on plain objects and arrays");
    }

    // Shape keys are arena-interned and immortal, so collecting them up
    // front is safe across the allocations below.
    Shape* shape = reinterpret_cast<ObjectHeader*>(hdr)->shape;
    std::vector<StringHeader*> inserted =
        shape ? shape->ownKeysInInsertionOrder() : std::vector<StringHeader*>{};

    // Spec order: integer-like keys ascending, then the rest in insertion
    // order. stable_partition would also work; an explicit split keeps the
    // numeric sort off the string keys.
    std::vector<std::pair<uint32_t, StringHeader*>> intKeys;
    std::vector<StringHeader*> strKeys;
    for (StringHeader* k : inserted) {
        uint32_t idx = 0;
        if (k->isLatin1() && isIntegerLikeKey(latin1View(k), idx)) {
            intKeys.emplace_back(idx, k);
        } else {
            strKeys.push_back(k);
        }
    }
    std::sort(intKeys.begin(), intKeys.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const uint32_t total = static_cast<uint32_t>(intKeys.size() + strKeys.size());
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(g_heap, total ? total : 4))};
    out.get().asObject<ArrayHeader>()->header.flags = 1;

    uint32_t at = 0;
    auto push = [&](StringHeader* name) {
        // Copy the immortal arena string into the heap: the result array
        // holds ordinary JS strings, not pointers into the shape arena.
        Rooted<Value> key{Value::fromString(
            StringHeader::createFromUTF8(g_heap, latin1View(name)))};
        out.get().asObject<ArrayHeader>()->setElem(g_heap, at++, key);
    };
    for (const auto& [idx, name] : intKeys) push(name);
    for (StringHeader* name : strKeys) push(name);

    return out.get().rawBits();
}

// Environment records (docs/0007). `depth` parent hops then `index`.
uint64_t bronze_env_create(uint64_t parentBits, uint32_t slotCount) {
    Rooted<Value> parent{Value(parentBits)};
    return Value::fromObject(EnvHeader::create(g_heap, parent, slotCount)).rawBits();
}

static EnvHeader* resolveEnv(uint64_t envBits, uint32_t depth) {
    Value envVal(envBits);
    if (!envVal.isObject()) {
        fatal("environment access on a value that is not an environment record");
    }
    auto* env = envVal.asObject<EnvHeader>();
    if (env->header.flags != EnvHeader::kFlags) {
        fatal("environment access on a value that is not an environment record");
    }
    return env->ancestor(depth);
}

uint64_t bronze_env_get(uint64_t envBits, uint32_t depth, uint32_t index) {
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    return env->slotsData()[index].rawBits();
}

void bronze_env_set(uint64_t envBits, uint32_t depth, uint32_t index, uint64_t valBits) {
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    env->slotsData()[index] = Value(valBits);
}

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint32_t icIndex) {
    Value objVal(objBits);

    // IC-hit fast path first: a shape match needs no key at all.
    if (objVal.isObject()) {
        HeapObjectHeader* fastHdr = objVal.asObject<HeapObjectHeader>();
        if (fastHdr->flags == 0 && icIndex < g_inlineCaches.size()) {
            const InlineCache& fastIc = g_inlineCaches[icIndex];
            auto* fastObj = reinterpret_cast<ObjectHeader*>(fastHdr);
            if (fastIc.cached_shape && fastIc.cached_shape == fastObj->shape) {
                // Depth 0 is an own property — the common case, straight to
                // the slot. Anything else was found up the prototype chain,
                // so the cached slot belongs to an ancestor and reading it
                // off the receiver would return a completely unrelated
                // property (docs/0008 decision 2).
                if (fastIc.cached_depth == 0) {
                    return fastObj->getSlot(fastIc.cached_slot).rawBits();
                }
                ObjectHeader* holder = fastObj->protoAncestor(fastIc.cached_depth);
                if (holder) {
                    return holder->getSlot(fastIc.cached_slot).rawBits();
                }
            }
        }
    }

    const std::string& keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : g_emptyKey;

    if (objVal.isString()) {
        StringHeader* str = objVal.asString<StringHeader>();
        if (keyStr == "length") {
            return Value::fromDouble(str->getLength()).rawBits();
        }
        if (keyStr == "charCodeAt") {
            if (g_charCodeAtFn.isUndefined()) {
                FunctionHeader* fn =
                    FunctionHeader::create(g_heap, bronze_builtin_string_char_code_at,
                                           Value::fromUndefined(), 1);
                fn->header.flags = 2;
                g_charCodeAtFn = Value::fromObject(fn);
                g_heap.add_permanent_root(&g_charCodeAtFn);
            }
            return g_charCodeAtFn.rawBits();
        }
        return Value::fromUndefined().rawBits();
    }

    if (!objVal.isObject()) {
        return Value::fromUndefined().rawBits();
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == 1) {
        // Array
        ArrayHeader* arr = reinterpret_cast<ArrayHeader*>(hdr);
        if (keyStr == "length") {
            return Value::fromDouble(arr->length).rawBits();
        }
        int idx = -1;
        auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), idx);
        if (ec == std::errc{} && idx >= 0) {
            return arr->getElem(static_cast<uint32_t>(idx)).rawBits();
        }
        return Value::fromUndefined().rawBits();
    }
    if (hdr->flags == 3) {
        // Float32Array view
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (keyStr == "length") {
            return Value::fromDouble(view->length).rawBits();
        }
        if (keyStr == "buffer") {
            return view->buffer.rawBits();
        }
        int idx = -1;
        auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), idx);
        if (ec == std::errc{} && idx >= 0) {
            if (static_cast<uint32_t>(idx) >= view->length) {
                return Value::fromUndefined().rawBits();
            }
            return Value::fromDouble(static_cast<double>(view->data()[idx])).rawBits();
        }
        return Value::fromUndefined().rawBits();
    }
    if (hdr->flags == 4) {
        // ArrayBuffer
        auto* buf = reinterpret_cast<ArrayBufferHeader*>(hdr);
        if (keyStr == "byteLength") {
            return Value::fromDouble(buf->byteLength).rawBits();
        }
        return Value::fromUndefined().rawBits();
    }
    if (hdr->flags == 2) {
        // Function. It carries a prototype slot rather than a shape, so
        // `prototype` is the one property it has; `name` and `length` are
        // not implemented (docs/0008 decision 5).
        if (keyStr == "prototype") {
            Rooted<Value> fnRoot{objVal};
            ensureFunctionPrototype(fnRoot);
            return fnRoot.get().asObject<FunctionHeader>()->prototype.rawBits();
        }
        return Value::fromUndefined().rawBits();
    }

    if (icIndex >= g_inlineCaches.size()) {
        g_inlineCaches.resize(icIndex + 1);
    }
    InlineCache* ic = &g_inlineCaches[icIndex];
    if (keyIndex >= g_keyHeaders.size() || !g_keyHeaders[keyIndex]) {
        fatal("property access with an unregistered key index");
    }
    // Interned arena key: no allocation on the property path.
    Rooted<Value> key(Value::fromString(g_keyHeaders[keyIndex]));
    ObjectHeader* obj = objVal.asObject<ObjectHeader>();
    Value result = obj->getProp(g_heap, key, ic);
    return result.rawBits();
}

void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint32_t icIndex) {
    Value objVal(objBits);
    Value valVal(valBits);
    if (!objVal.isObject()) return;

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // IC-hit fast path: a shape match writes the slot with no key and no
    // rooting (nothing below can allocate).
    if (hdr->flags == 0 && icIndex < g_inlineCaches.size()) {
        const InlineCache& fastIc = g_inlineCaches[icIndex];
        auto* fastObj = reinterpret_cast<ObjectHeader*>(hdr);
        if (fastIc.cached_shape && fastIc.cached_shape == fastObj->shape) {
            fastObj->setSlot(fastIc.cached_slot, valVal);
            return;
        }
    }

    const std::string& keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : g_emptyKey;

    if (hdr->flags == 1) {
        // Array
        ArrayHeader* arr = reinterpret_cast<ArrayHeader*>(hdr);
        int idx = -1;
        auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), idx);
        if (ec == std::errc{} && idx >= 0) {
            Rooted<Value> val(valVal);
            arr->setElem(g_heap, static_cast<uint32_t>(idx), val);
        }
        return;
    }
    if (hdr->flags == 3) {
        // Float32Array view: numeric keys store an element (out-of-bounds
        // writes are discarded, per spec); anything else is unsupported.
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        int idx = -1;
        auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), idx);
        if (ec != std::errc{} || idx < 0) {
            fatal("named property writes on a Float32Array are unsupported");
        }
        if (static_cast<uint32_t>(idx) < view->length) {
            view->data()[idx] = static_cast<float>(bronze_unbox_f64(valBits));
        }
        return;
    }
    if (hdr->flags == 4) {
        fatal("property writes on an ArrayBuffer are unsupported");
    }
    if (hdr->flags == 2) {
        // A function has a prototype slot, not a shape, so `prototype` is
        // the only property it can be given. Anything else is named rather
        // than dropped (docs/0008 decision 5).
        if (keyStr != "prototype") {
            fatal("property writes on a function object other than `prototype` are "
                  "unsupported until functions carry shapes");
        }
        if (!valVal.isObject()) {
            fatal("assigning a non-object to a function's `prototype` is unsupported");
        }
        auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
        fn->prototype = valVal;
        // Instances made from here on get the new prototype; ones already
        // made keep their shape, and so keep the old one.
        fn->instance_shape = newRootShape(valVal);
        return;
    }

    if (icIndex >= g_inlineCaches.size()) {
        g_inlineCaches.resize(icIndex + 1);
    }
    InlineCache* ic = &g_inlineCaches[icIndex];
    if (keyIndex >= g_keyHeaders.size() || !g_keyHeaders[keyIndex]) {
        fatal("property write with an unregistered key index");
    }
    // Interned arena key: no allocation before the object is dereferenced.
    // setProp itself may still allocate (overflow growth); it re-derives
    // the object through its own root, but this caller's objBits raw value
    // is dead after the call, so that is safe.
    Rooted<Value> key(Value::fromString(g_keyHeaders[keyIndex]));
    Rooted<Value> val(valVal);
    ObjectHeader* obj = objVal.asObject<ObjectHeader>();
    obj->setProp(g_heap, g_arena, key, val, ic);
}

uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    Value calleeVal(calleeBits);
    Value thisVal(thisBits);
    char msg[128];
    if (!calleeVal.isObject()) {
        std::snprintf(msg, sizeof(msg), "attempted to call a non-object value (bits %016llx)",
                      static_cast<unsigned long long>(calleeBits));
        fatal(msg);
    }
    HeapObjectHeader* hdr = calleeVal.asObject<HeapObjectHeader>();
    if (hdr->flags != 2) {
        std::snprintf(msg, sizeof(msg), "attempted to call a non-function object (flags=%u)",
                      static_cast<unsigned>(hdr->flags));
        fatal(msg);
    }
    FunctionHeader* fn = reinterpret_cast<FunctionHeader*>(hdr);
    // argvBits already points into the caller's GC root frame (docs/0006),
    // so it is rooted exactly as long as the call needs it. Copying it into
    // a vector, as this used to, built an *unrooted* duplicate — and cost a
    // malloc on every dynamic call for the privilege.
    Value* argv = reinterpret_cast<Value*>(const_cast<uint64_t*>(argvBits));
    Value res = fn->call(thisVal, argc, argv);
    return res.rawBits();
}

uint64_t bronze_string_concat(uint64_t aBits, uint64_t bBits) {
    Value aVal(aBits);
    Value bVal(bBits);
    Rooted<Value> aRoot(valueToString(aVal));
    Rooted<Value> bRoot(valueToString(bVal));
    Value res = StringHeader::concat(g_heap, aRoot, bRoot);
    return res.rawBits();
}

uint64_t bronze_dynamic_add(uint64_t aBits, uint64_t bBits) {
    Value aVal(aBits);
    Value bVal(bBits);
    if (aVal.isString() || bVal.isString()) {
        Rooted<Value> aRoot(valueToString(aVal));
        Rooted<Value> bRoot(valueToString(bVal));
        Value res = StringHeader::concat(g_heap, aRoot, bRoot);
        return res.rawBits();
    }
    double aNum = bronze_unbox_f64(aBits);
    double bNum = bronze_unbox_f64(bBits);
    return Value::fromDouble(aNum + bNum).rawBits();
}

void bronze_print_value(uint64_t valBits) {
    Value v(valBits);
    if (v.isNumber()) {
        double num = v.asNumber();
        char buf[64];
        size_t len = 0;
        // console.log distinguishes -0 (inspect formatting), unlike
        // ToString(Number) which yields "0" — node prints "-0" here.
        if (num == 0.0 && std::signbit(num)) {
            buf[len++] = '-';
        }
        len += formatJsNumber(num, buf + len);
        buf[len++] = '\n';
        std::fwrite(buf, 1, len, stdout);
    } else if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        if (str->isLatin1()) {
            const char* data = str->latin1Data();
            uint32_t len = str->getLength();
            for (uint32_t i = 0; i < len; ++i) {
                unsigned char c = static_cast<unsigned char>(data[i]);
                if (c <= 0x7F) {
                    std::fputc(c, stdout);
                } else {
                    std::fputc(static_cast<char>(0xC0 | (c >> 6)), stdout);
                    std::fputc(static_cast<char>(0x80 | (c & 0x3F)), stdout);
                }
            }
        } else {
            const uint16_t* u16 = str->utf16Data();
            uint32_t len = str->getLength();
            for (uint32_t i = 0; i < len; ++i) {
                uint32_t cp = u16[i];
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
                    uint32_t low = u16[i + 1];
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i++;
                    }
                }
                if (cp <= 0x7F) {
                    std::fputc(static_cast<char>(cp), stdout);
                } else if (cp <= 0x7FF) {
                    std::fputc(static_cast<char>(0xC0 | (cp >> 6)), stdout);
                    std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
                } else if (cp <= 0xFFFF) {
                    std::fputc(static_cast<char>(0xE0 | (cp >> 12)), stdout);
                    std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), stdout);
                    std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
                } else {
                    std::fputc(static_cast<char>(0xF0 | (cp >> 18)), stdout);
                    std::fputc(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), stdout);
                    std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), stdout);
                    std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
                }
            }
        }
        std::fputc('\n', stdout);
    } else if (v.isBool()) {
        const char* s = v.asBool() ? "true\n" : "false\n";
        std::fputs(s, stdout);
    } else if (v.isUndefined()) {
        std::fputs("undefined\n", stdout);
    } else {
        std::fputs("[object]\n", stdout);
    }
    std::fflush(stdout);
}

void bronze_print_string(const char* s) {
    if (s) {
        std::fputs(s, stdout);
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// A computed index must be a non-negative integral number; anything else
// on the supported receivers reads as undefined / discards the write.
static bool valueToElementIndex(Value idxVal, uint32_t& out) {
    if (!idxVal.isNumber()) {
        fatal("computed index must be a number (string/object keys in [] are unsupported)");
    }
    double d = idxVal.asNumber();
    if (!(d >= 0.0) || d != std::floor(d) || d > 4294967294.0) {
        return false;
    }
    out = static_cast<uint32_t>(d);
    return true;
}

uint64_t bronze_elem_get(uint64_t objBits, uint64_t idxBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("computed index access on a non-object value is unsupported");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;
    if (hdr->flags == 1) {
        if (!valueToElementIndex(Value(idxBits), idx)) {
            return Value::fromUndefined().rawBits();
        }
        return reinterpret_cast<ArrayHeader*>(hdr)->getElem(idx).rawBits();
    }
    if (hdr->flags == 3) {
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (!valueToElementIndex(Value(idxBits), idx) || idx >= view->length) {
            return Value::fromUndefined().rawBits();
        }
        return Value::fromDouble(static_cast<double>(view->data()[idx])).rawBits();
    }
    fatal("computed index access is only supported on arrays and Float32Array");
}

void bronze_elem_set(uint64_t objBits, uint64_t idxBits, uint64_t valBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("computed index write on a non-object value is unsupported");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;
    if (hdr->flags == 1) {
        if (!valueToElementIndex(Value(idxBits), idx)) {
            fatal("non-integer array index write is unsupported");
        }
        Rooted<Value> val{Value(valBits)};
        reinterpret_cast<ArrayHeader*>(hdr)->setElem(g_heap, idx, val);
        return;
    }
    if (hdr->flags == 3) {
        auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
        if (valueToElementIndex(Value(idxBits), idx) && idx < view->length) {
            view->data()[idx] = static_cast<float>(bronze_unbox_f64(valBits));
        }
        return;  // out-of-bounds typed-array writes are discarded, per spec
    }
    fatal("computed index writes are only supported on arrays and Float32Array");
}

uint64_t bronze_create_arraybuffer(uint64_t lenBits) {
    Value lenVal(lenBits);
    if (!lenVal.isNumber()) {
        fatal("new ArrayBuffer requires a numeric byte length");
    }
    double d = lenVal.asNumber();
    if (!(d >= 0.0) || d != std::floor(d) || d > 268435456.0) {
        fatal("invalid ArrayBuffer byte length");
    }
    return Value::fromObject(ArrayBufferHeader::create(g_heap, static_cast<uint32_t>(d)))
        .rawBits();
}

uint64_t bronze_create_float32array(uint64_t argBits) {
    Value arg(argBits);
    if (arg.isNumber()) {
        double d = arg.asNumber();
        if (!(d >= 0.0) || d != std::floor(d) || d > 67108864.0) {
            fatal("invalid Float32Array length");
        }
        return Value::fromObject(Float32ArrayHeader::create(g_heap, static_cast<uint32_t>(d)))
            .rawBits();
    }
    if (arg.isObject() && arg.asObject<HeapObjectHeader>()->flags == 4) {
        Rooted<Value> bufRoot(arg);
        return Value::fromObject(Float32ArrayHeader::createOverBuffer(g_heap, bufRoot)).rawBits();
    }
    fatal("new Float32Array requires a length or an ArrayBuffer");
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
