// The `Reflect` namespace (ECMA-262 28.1).
//
// Its members ARE the essential internal methods, one function per method,
// which is why it is a file of its own rather than a corner of rt_object.cpp:
// every one of them is the same operation an ordinary expression performs
// (`Reflect.get` is `o[k]`, `Reflect.has` is `k in o`), so each body is a
// forward to the funnel that already implements it, and the value of the file
// is that the nine forwards sit together where a tenth can be checked against
// them.
//
// It is also the naming authority for Proxy: a trap is named after the
// Reflect member of the same operation, and a handler that wants default
// behaviour calls the Reflect member. Keeping the list here keeps that
// correspondence in one place.
//
// Two members carry a named refusal rather than a forward. `Reflect.get` and
// `Reflect.set` take a RECEIVER distinct from the target (28.1.6 step 4,
// 28.1.13 step 5), which decides what `this` a getter or setter found on the
// target runs against; bronze's read and write funnels bind the receiver to
// the object they were given, so a distinct one is refused instead of
// silently ignored.

#include <cstdint>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {


static uint64_t reflectApply(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 3) {
        return rtThrowTypeError("Reflect.apply requires at least 3 arguments").rawBits();
    }
    Value target(argv[0]);
    Value thisArg(argv[1]);
    Value argsList(argv[2]);
    if (!target.isObject() || target.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError("Reflect.apply: target must be a function").rawBits();
    }
    if (!argsList.isObject()) {
        // 28.1.1 step 2's CreateListFromArrayLike (7.3.19) step 1: a primitive
        // argument list is a TypeError, and `null` is not the "no arguments"
        // spelling here that it is for `Function.prototype.apply` — 20.2.3.1
        // has an extra step for it that 28.1.1 does not.
        return rtThrowTypeError("Reflect.apply: arguments must be an object").rawBits();
    }
    // 7.3.19 over ANY object with a `length`, the same walk
    // `Function.prototype.apply` takes. Both reads can run a getter, so the
    // list is held through a root and the argument block is rooted with it.
    Rooted<Value> targetRoot{target};
    Rooted<Value> thisRoot{thisArg};
    Rooted<Value> listRoot{argsList};
    const uint32_t count = rtArrayLikeLength(listRoot);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (!rtCheckAppliedArgumentCount(count, "Reflect.apply")) {
        return Value::fromUndefined().rawBits();
    }
    RootedBlock block(count);
    for (uint32_t i = 0; i < count; ++i) {
        block.set(i, rtArrayLikeElement(listRoot, i));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return bronze_dynamic_call(targetRoot.get().rawBits(), thisRoot.get().rawBits(), count,
                               block.data());
}

static uint64_t reflectGet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 2) return BRONZE_ABI_UNDEFINED_BITS;
    // 28.1.6 step 4: with a receiver, a GETTER found on the target runs
    // against the receiver instead. bronze's read path would run it against
    // the target, so a distinct receiver is refused rather than misanswered.
    if (argc > 2 && argv[2] != argv[0]) {
        fatal("unsupported: Reflect.get with a receiver distinct from the target");
    }
    return bronze_elem_get(argv[0], argv[1]);
}

static uint64_t reflectSet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 3) return Value::fromBool(false).rawBits();
    if (argc > 3 && argv[3] != argv[0]) {
        fatal("unsupported: Reflect.set with a receiver distinct from the target");
    }
    bronze_elem_set(argv[0], argv[1], argv[2], /*strict=*/false);
    return Value::fromBool(true).rawBits();
}

static uint64_t reflectHas(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 2) return Value::fromBool(false).rawBits();
    return Value::fromBool(bronze_has_property(argv[1], argv[0])).rawBits();
}

static uint64_t reflectOwnKeys(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc == 0) return rtThrowTypeError("Reflect.ownKeys called on non-object").rawBits();
    const uint64_t call[1] = {argv[0]};
    return rtObjectGetOwnPropertyNames(0, 0, 1, call);
}

static uint64_t reflectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return objectGetPrototypeOf(0, 0, argc, argv);
}

static uint64_t reflectSetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return objectSetPrototypeOf(0, 0, argc, argv);
}

static uint64_t reflectGetOwnPropertyDescriptor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return rtObjectGetOwnPropertyDescriptor(0, 0, argc, argv);
}

static uint64_t reflectDefineProperty(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return rtObjectDefineProperty(0, 0, argc, argv);
}

static Value g_reflectNamespace = Value::fromUndefined();

Value rtReflectNamespace() {
    if (g_reflectNamespace.isUndefined()) {
        Rooted<Value> ns{Value::fromObject(
            ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(Value::fromNull())))};
        ns.get().asObject<HeapObjectHeader>()->flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        g_reflectNamespace = ns.get();
        rtHeap().add_permanent_root(&g_reflectNamespace);

        const NativeMethod methods[] = {
            {"apply", reflectApply, 3},
            {"get", reflectGet, 2},
            {"set", reflectSet, 3},
            {"has", reflectHas, 2},
            {"ownKeys", reflectOwnKeys, 1},
            {"getPrototypeOf", reflectGetPrototypeOf, 1},
            {"setPrototypeOf", reflectSetPrototypeOf, 2},
            {"getOwnPropertyDescriptor", reflectGetOwnPropertyDescriptor, 2},
            {"defineProperty", reflectDefineProperty, 3},
        };
        rtDefineMethods(ns, methods, std::size(methods));
    }
    return g_reflectNamespace;
}
}  // namespace bronze::runtime
