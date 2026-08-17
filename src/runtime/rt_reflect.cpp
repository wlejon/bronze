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
// `Reflect.get` and `Reflect.set` are the two that are more than a forward.
// Each takes a RECEIVER distinct from the target (28.1.6 step 4, 28.1.13 step
// 5), which decides what `this` a getter or setter found on the target runs
// against — and bronze's ABI read and write funnels bind the receiver to the
// object they were handed. So those two dispatch on WHERE an accessor could be
// stored and reach `ObjectHeader::getProp`/`setProp` (which have taken a
// receiver since `super.k` needed one) or the proxy trap directly. The enum
// below names the four cases and says why the fourth cannot observe a
// receiver at all.

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
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
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

// Where a receiver DISTINCT from the target can be observed, and where it
// cannot.
//
// It is observable exactly where an accessor can be stored, because binding
// `this` is the only thing a receiver does. That is a PLAIN object's shape, a
// FUNCTION's statics object (which is a plain object, and is what
// `Object.defineProperty` on a function writes into), and a PROXY — whose trap
// is handed the receiver as an argument and may do anything with it.
//
// Every other kind stores no accessor: an array's, a Map's and a typed array's
// members are answered from C tables beside the value (rt_prop.cpp), and
// `Object.defineProperty` refuses all of them by name, so there is no route by
// which one could hold a getter for a receiver to matter to. For those the
// receiver is not ignored — it is unobservable, and the forward is exact.
enum class ReceiverRoute {
    Plain,     // the receiver reaches ObjectHeader::getProp / setProp
    Function,  // ... after stepping onto the statics object
    Proxy,     // ... as the trap's own argument
    Unobservable,
};

static ReceiverRoute receiverRoute(Value target) {
    if (!target.isObject()) return ReceiverRoute::Unobservable;
    switch (target.asObject<HeapObjectHeader>()->flags) {
        case BRONZE_ABI_OBJ_FLAGS_PLAIN: return ReceiverRoute::Plain;
        case HeapKind::Function: return ReceiverRoute::Function;
        case HeapKind::Proxy: return ReceiverRoute::Proxy;
        default: return ReceiverRoute::Unobservable;
    }
}

// 10.1.9.2 OrdinarySetWithOwnDescriptor, reduced to the one question bronze's
// write funnel cannot answer for itself: WHERE does a write with a distinct
// receiver land?
//
// Step 3 sends an accessor write to the setter the target's chain holds, with
// the receiver as `this` — which the funnel has done since `super.k = v` needed
// it. Step 2 sends a DATA write somewhere else entirely: to the RECEIVER, as a
// new own property. `Reflect.set(o, 'a', 9, other)` leaves `o.a` at 1 and gives
// `other` an `a` of 9, and the funnel would have written `o.a`. So the
// destination is decided here and then dispatched to.
//
// The walk is the chain walk of step 1, and it stops at a kind whose prototype
// cannot be asked for; that is not a shortcut — a kind with no shape holds no
// accessor either (the enum above says why), so the answer past that point is
// step 1.d's, which is this one.
enum class SetDestination {
    Accessor,           // the target's chain has a setter; the funnel runs it
    DataOntoReceiver,   // step 2: a new own property of the receiver
    Refused,            // a non-writable data property, or a pending exception
};

static SetDestination setDestination(Rooted<Value>& target, Rooted<Value>& key) {
    Rooted<Value> holder{target.get()};
    for (uint32_t depth = 0; depth < 64; ++depth) {
        OwnPropertyDetail found;
        const bool own = rtOwnPropertyOf(holder, key.get(), found);
        if (rtExceptionPending()) return SetDestination::Refused;
        if (own) {
            if (found.accessor) return SetDestination::Accessor;
            return found.writable ? SetDestination::DataOntoReceiver : SetDestination::Refused;
        }
        if (receiverRoute(holder.get()) == ReceiverRoute::Unobservable) break;
        const uint64_t call[1] = {holder.get().rawBits()};
        Value proto = Value(objectGetPrototypeOf(0, 0, 1, call));
        if (rtExceptionPending()) return SetDestination::Refused;
        if (!proto.isObject()) break;  // step 1.d: the absent-everywhere case
        holder.set(proto);
    }
    return SetDestination::DataOntoReceiver;
}

// Step 2's own last three tests, asked of the RECEIVER: an own accessor or an
// own non-writable data property there refuses the write rather than taking it.
static bool receiverAcceptsData(Rooted<Value>& receiver, Rooted<Value>& key) {
    if (!receiver.get().isObject()) return false;  // step 2.b
    OwnPropertyDetail existing;
    if (!rtOwnPropertyOf(receiver, key.get(), existing)) return !rtExceptionPending();
    if (rtExceptionPending()) return false;
    return !existing.accessor && existing.writable;
}

// 28.1.6 Reflect.get(target, propertyKey, receiver).
static uint64_t reflectGet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 2) return BRONZE_ABI_UNDEFINED_BITS;
    // Step 1. A primitive target has no [[Get]] to call, and answering
    // `undefined` for one would make `Reflect.get(null, 'x')` quieter than
    // `null.x`.
    if (!Value(argv[0]).isObject()) {
        return rtThrowTypeError("Reflect.get called on a value that is not an object").rawBits();
    }
    // Step 3: the receiver defaults to the target, which is the case every
    // ordinary property read is.
    if (argc <= 2 || argv[2] == argv[0]) return bronze_elem_get(argv[0], argv[1]);

    Rooted<Value> target{Value(argv[0])};
    Rooted<Value> key{Value(argv[1])};
    Rooted<Value> receiver{Value(argv[2])};
    switch (receiverRoute(target.get())) {
        case ReceiverRoute::Proxy:
            return rtProxyGet(target.get(), key.get(), receiver.get()).rawBits();
        case ReceiverRoute::Function: {
            // A function's own `name`, `length` and `prototype` are data
            // properties in the header, so only the statics object can hold an
            // accessor — and reaching it directly is what makes the receiver
            // arrive at `getProp`. It is created on demand, so a function that
            // has never had a static has nothing to read and the target's own
            // answer stands.
            Value props = target.get().asObject<FunctionHeader>()->properties;
            if (!props.isObject()) return bronze_elem_get(argv[0], argv[1]);
            Rooted<Value> holder{props};
            key.set(rtToPropertyKey(key));
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            return holder.get()
                .asObject<ObjectHeader>()
                ->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr())
                .rawBits();
        }
        case ReceiverRoute::Plain:
            // No inline cache, for `bronze_super_get`'s reason: an entry
            // describes ONE shape and this read has two objects.
            key.set(rtToPropertyKey(key));
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            return target.get()
                .asObject<ObjectHeader>()
                ->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr())
                .rawBits();
        case ReceiverRoute::Unobservable:
            return bronze_elem_get(target.get().rawBits(), key.get().rawBits());
    }
    return BRONZE_ABI_UNDEFINED_BITS;
}

// 28.1.13 Reflect.set(target, propertyKey, V, receiver).
static uint64_t reflectSet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 3) return Value::fromBool(false).rawBits();
    if (!Value(argv[0]).isObject()) {
        return rtThrowTypeError("Reflect.set called on a value that is not an object").rawBits();
    }
    if (argc <= 3 || argv[3] == argv[0]) {
        bronze_elem_set(argv[0], argv[1], argv[2], /*strict=*/false);
        return Value::fromBool(!rtExceptionPending()).rawBits();
    }

    Rooted<Value> target{Value(argv[0])};
    Rooted<Value> key{Value(argv[1])};
    Rooted<Value> val{Value(argv[2])};
    Rooted<Value> receiver{Value(argv[3])};
    key.set(rtToPropertyKey(key));
    if (rtExceptionPending()) return Value::fromBool(false).rawBits();

    if (receiverRoute(target.get()) == ReceiverRoute::Proxy) {
        // 10.5.9 hands the receiver to the trap and stops there: a proxy's
        // [[Set]] is the trap, not OrdinarySet, so none of the chain walk below
        // applies to one.
        rtProxySet(target.get(), key.get(), val.get(), /*strict=*/false, receiver.get());
        return Value::fromBool(!rtExceptionPending()).rawBits();
    }

    switch (setDestination(target, key)) {
        case SetDestination::Refused:
            return Value::fromBool(false).rawBits();
        case SetDestination::DataOntoReceiver: {
            if (!receiverAcceptsData(receiver, key)) return Value::fromBool(false).rawBits();
            bronze_elem_set(receiver.get().rawBits(), key.get().rawBits(),
                            val.get().rawBits(), /*strict=*/false);
            return Value::fromBool(!rtExceptionPending()).rawBits();
        }
        case SetDestination::Accessor:
            break;
    }

    // The setter, run by the funnel with the receiver bound as `this`. A
    // FUNCTION target keeps its writable properties in the statics object, and
    // stepping onto it is what puts the receiver in front of `setProp`.
    Rooted<Value> holder{target.get()};
    if (receiverRoute(target.get()) == ReceiverRoute::Function) {
        Value props = target.get().asObject<FunctionHeader>()->properties;
        if (!props.isObject()) return Value::fromBool(false).rawBits();
        holder.set(props);
    }
    SetRefusal refusal = SetRefusal::None;
    holder.get().asObject<ObjectHeader>()->setProp(
        rtHeap(), rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
        /*defineOwn=*/false, receiver.slot_ptr(), &refusal);
    // 28.1.13 answers the BOOLEAN [[Set]] returned, and for the accessor arm
    // the funnel does report it: a setter-less accessor is 10.1.9.2 step 3.b's
    // false.
    return Value::fromBool(refusal == SetRefusal::None && !rtExceptionPending()).rawBits();
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

static thread_local Value g_reflectNamespace = Value::fromUndefined();

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
