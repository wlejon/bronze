// Object, array, function and typed-array construction, the two class links,
// environment records, and the dynamic call path. Property access itself is
// rt_prop.cpp.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/env.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The kind of a value, named. Diagnostics print this rather than raw bits:
// "attempted to call undefined" bisects to a construct, `fff6000000000000`
// bisects to nothing.
static const char* valueKindName(Value v) {
    if (v.isNumber() || v.isInt32()) return "a number";
    if (v.isString()) return "a string";
    if (v.isBool()) return "a boolean";
    if (v.isNull()) return "null";
    if (v.isUndefined()) return "undefined";
    if (v.isHole()) return "the internal hole sentinel";
    if (v.isSymbol()) return "a symbol";
    if (v.isObject()) {
        switch (v.asObject<HeapObjectHeader>()->flags) {
            case 1: return "an array";
            case 2: return "a function";
            case 3: return "a Float32Array";
            case 4: return "an ArrayBuffer";
            default: return "an object";
        }
    }
    return "a value of an unknown kind";
}

static std::string_view latin1View(const StringHeader* s) {
    return std::string_view(s->latin1Data(), s->getLength());
}

// A function's `.prototype` serves both as a constructor's instance prototype
// and as the target of `Foo.prototype.m = ...`, and those must be the same
// object, so both go through here (docs/0008 decision 4).
void rtEnsureFunctionPrototype(Rooted<Value>& fnVal) {
    FunctionHeader* fn = fnVal.get().asObject<FunctionHeader>();
    if (fn->prototype.isObject() && fn->instance_shape) return;

    ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    proto->header.flags = 0;

    fn = fnVal.get().asObject<FunctionHeader>();  // create() may have moved it
    fn->prototype = Value::fromObject(proto);
    fn->instance_shape = rtNewRootShape(fn->prototype);
}

void rtEnsureFunctionProperties(Rooted<Value>& fnVal) {
    FunctionHeader* fn = fnVal.get().asObject<FunctionHeader>();
    if (fn->properties.isObject()) return;
    ObjectHeader* props = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    props->header.flags = 0;
    fn = fnVal.get().asObject<FunctionHeader>();  // create() may have moved it
    fn->properties = Value::fromObject(props);
}

// Spec order for a plain object's own ENUMERABLE string keys, in one place
// because four callers need the same answer and a second copy of the
// integer-first split would be a second chance to get `"10"` before `"2"`
// wrong (docs/0009). Enumerable-only because every caller — `Object.keys`,
// object spread, object rest and `for-in` — is defined over own enumerable
// keys, and a class method is not one (docs/0018 decision 2).
std::vector<StringHeader*> rtOwnKeysOrdered(const ObjectHeader* obj) {
    // Shape keys are arena-interned and immortal, so collecting them up front
    // is safe across whatever the caller allocates while walking them.
    Shape* shape = obj->shape;
    std::vector<StringHeader*> inserted =
        shape ? shape->ownKeysInInsertionOrder(/*enumerableOnly=*/true)
              : std::vector<StringHeader*>{};

    std::vector<std::pair<uint32_t, StringHeader*>> intKeys;
    std::vector<StringHeader*> strKeys;
    for (StringHeader* k : inserted) {
        uint32_t idx = 0;
        if (k->isLatin1() && rtIsIntegerLikeKey(latin1View(k), idx)) {
            intKeys.emplace_back(idx, k);
        } else {
            strKeys.push_back(k);
        }
    }
    std::sort(intKeys.begin(), intKeys.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<StringHeader*> ordered;
    ordered.reserve(intKeys.size() + strKeys.size());
    for (const auto& [idx, name] : intKeys) ordered.push_back(name);
    for (StringHeader* name : strKeys) ordered.push_back(name);
    return ordered;
}

Value rtCopyKeyToHeap(const StringHeader* key) {
    if (key->isLatin1()) {
        return Value::fromString(
            StringHeader::createLatin1(rtHeap(), key->latin1Data(), key->getLength()));
    }
    return Value::fromString(
        StringHeader::createUTF16(rtHeap(), key->utf16Data(), key->getLength()));
}

extern "C" {

uint64_t bronze_create_object() {
    ObjectHeader* obj = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    obj->header.flags = 0;
    return Value::fromObject(obj).rawBits();
}

uint64_t bronze_create_array(uint32_t length) {
    uint32_t cap = (length < 4) ? 4 : length;
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), cap);
    arr->header.flags = 1;
    arr->length = length;
    return Value::fromObject(arr).rawBits();
}

uint64_t bronze_create_function(bronze_fn_code code, uint32_t arity, uint64_t envBits) {
    Rooted<Value> env{Value(envBits)};
    FunctionHeader* fn = FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity);
    // Read the environment through the root only AFTER allocating: the
    // allocation above can collect, and a by-value copy taken before it would
    // point into dead from-space.
    fn->env_record = env.get();
    fn->header.flags = 2;
    return Value::fromObject(fn).rawBits();
}

// `class D extends B` — the two prototype links a class sets up, and the only
// runtime concept classes add (docs/0012 decision 5). Instances: D.prototype's
// proto is B.prototype, so an inherited method is found by the ordinary chain
// walk. Statics: D's own-property object's proto is B's, so `D.staticOfB()`
// resolves the same way.
//
// D.prototype is REPLACED rather than mutated, because the prototype lives on
// the shape (docs/0004) — which is also why this must run before any method is
// stored on it.
void bronze_class_extends(uint64_t derivedBits, uint64_t baseBits) {
    Value derivedVal(derivedBits);
    Value baseVal(baseBits);
    if (!derivedVal.isObject() || derivedVal.asObject<HeapObjectHeader>()->flags != 2) {
        fatal("internal: class extends with a non-function derived class");
    }
    if (!baseVal.isObject() || baseVal.asObject<HeapObjectHeader>()->flags != 2) {
        fatal("a class can only extend another class or a constructor function");
    }

    Rooted<Value> derived{derivedVal};
    Rooted<Value> base{baseVal};
    rtEnsureFunctionPrototype(base);
    rtEnsureFunctionProperties(base);

    Rooted<Value> baseProto{base.get().asObject<FunctionHeader>()->prototype};
    ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(baseProto.get()));
    proto->header.flags = 0;
    Rooted<Value> protoRoot{Value::fromObject(proto)};

    Rooted<Value> baseProps{base.get().asObject<FunctionHeader>()->properties};
    ObjectHeader* props = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(baseProps.get()));
    props->header.flags = 0;

    FunctionHeader* fn = derived.get().asObject<FunctionHeader>();
    fn->prototype = protoRoot.get();
    fn->properties = Value::fromObject(props);
    fn->instance_shape = rtNewRootShape(protoRoot.get());
}

uint64_t bronze_construct(uint64_t fnBits, uint32_t argc, const uint64_t* argvBits) {
    Value fnVal(fnBits);
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != 2) {
        return rtThrowTypeError(std::string(valueKindName(fnVal)) + " is not a constructor")
            .rawBits();
    }

    Rooted<Value> fnRoot{fnVal};
    rtEnsureFunctionPrototype(fnRoot);

    FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
    ObjectHeader* instance = ObjectHeader::create(rtHeap(), rtArena(), fn->instance_shape);
    instance->header.flags = 0;

    Rooted<Value> self{Value::fromObject(instance)};
    // argv is the caller's rooted frame slots (docs/0006), so the argument
    // values stay live across the callee's allocations without a copy.
    fn = fnRoot.get().asObject<FunctionHeader>();
    Value result = fn->call(self.get(), argc,
                            const_cast<Value*>(reinterpret_cast<const Value*>(argvBits)));

    // JS: a constructor returning an object replaces the instance; any other
    // return value (including undefined) is ignored.
    return result.isObject() ? result.rawBits() : self.get().rawBits();
}

uint64_t bronze_object_keys(uint64_t objBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("Object.keys on a value that is not an object");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // An array's own keys are its indices, already in ascending order — the
    // ones it actually HAS: a hole left by `delete a[i]` is not an own
    // property, so the result is shorter than `length` (docs/0019 dec. 2).
    if (hdr->flags == 1) {
        Rooted<Value> src{objVal};
        uint32_t length = reinterpret_cast<ArrayHeader*>(hdr)->length;
        Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), length ? length : 4))};
        out.get().asObject<ArrayHeader>()->header.flags = 1;
        uint32_t at = 0;
        for (uint32_t i = 0; i < length; ++i) {
            if (!src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            char buf[16];
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), i);
            Rooted<Value> key{Value::fromString(
                StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, end - buf)))};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    if (hdr->flags != 0) {
        fatal("Object.keys is only supported on plain objects and arrays");
    }

    const std::vector<StringHeader*> ordered =
        rtOwnKeysOrdered(reinterpret_cast<ObjectHeader*>(hdr));

    const uint32_t total = static_cast<uint32_t>(ordered.size());
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), total ? total : 4))};
    out.get().asObject<ArrayHeader>()->header.flags = 1;

    uint32_t at = 0;
    for (StringHeader* name : ordered) {
        // Copy the immortal arena string into the heap: the result array holds
        // ordinary JS strings, not pointers into the shape arena.
        Rooted<Value> key{rtCopyKeyToHeap(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }

    return out.get().rawBits();
}

// ---- Environment records (docs/0007): `depth` parent hops, then `index` ----

uint64_t bronze_env_create(uint64_t parentBits, uint32_t slotCount) {
    Rooted<Value> parent{Value(parentBits)};
    return Value::fromObject(EnvHeader::create(rtHeap(), parent, slotCount)).rawBits();
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

// ---- Typed arrays -----------------------------------------------------------

uint64_t bronze_create_arraybuffer(uint64_t lenBits) {
    Value lenVal(lenBits);
    if (!lenVal.isNumber()) {
        fatal("new ArrayBuffer requires a numeric byte length");
    }
    double d = lenVal.asNumber();
    if (!(d >= 0.0) || d != std::floor(d) || d > 268435456.0) {
        fatal("invalid ArrayBuffer byte length");
    }
    return Value::fromObject(ArrayBufferHeader::create(rtHeap(), static_cast<uint32_t>(d)))
        .rawBits();
}

uint64_t bronze_create_float32array(uint64_t argBits) {
    Value arg(argBits);
    if (arg.isNumber()) {
        double d = arg.asNumber();
        if (!(d >= 0.0) || d != std::floor(d) || d > 67108864.0) {
            fatal("invalid Float32Array length");
        }
        return Value::fromObject(Float32ArrayHeader::create(rtHeap(), static_cast<uint32_t>(d)))
            .rawBits();
    }
    if (arg.isObject() && arg.asObject<HeapObjectHeader>()->flags == 4) {
        Rooted<Value> bufRoot(arg);
        return Value::fromObject(Float32ArrayHeader::createOverBuffer(rtHeap(), bufRoot)).rawBits();
    }
    fatal("new Float32Array requires a length or an ArrayBuffer");
}

uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc,
                             const uint64_t* argvBits) {
    Value calleeVal(calleeBits);
    if (!calleeVal.isObject() || calleeVal.asObject<HeapObjectHeader>()->flags != 2) {
        return rtThrowTypeError(std::string(valueKindName(calleeVal)) + " is not a function")
            .rawBits();
    }
    auto* fn = calleeVal.asObject<FunctionHeader>();
    // argvBits already points into the caller's GC root frame (docs/0006), so
    // it is rooted exactly as long as the call needs it — copying it into a
    // vector would build an *unrooted* duplicate and cost a malloc per call.
    Value* argv = reinterpret_cast<Value*>(const_cast<uint64_t*>(argvBits));
    return fn->call(Value(thisBits), argc, argv).rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
