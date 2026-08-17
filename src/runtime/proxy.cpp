// The Proxy exotic object's internal methods (ECMA-262 10.5), plus the
// constructor and 28.2.2.1's revocable pair. The receiver-generic
// Array.prototype methods a proxy over an ARRAY needs are the other half and
// live in proxy_array.cpp.
//
// Two rules run through everything here.
//
// A TRAP IS FOUND WITH GetMethod, never with a shape read: 10.5's internal
// methods all begin `GetMethod(handler, "trap")`, which is an ordinary [[Get]]
// through the handler's whole chain followed by a callable check. That is what
// makes any object a legal handler, an inherited trap findable, and a handler
// with no trap for an operation a forwarder rather than a refusal.
//
// EVERY OPERATION IS A GC POINT AND A THROW POINT. A trap is user code: it can
// allocate, it can collect, and it can throw. So every value a step still needs
// afterwards is rooted before the call, and every step tests the pending cell
// before it uses a result.

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"

namespace bronze {

ProxyHeader* ProxyHeader::create(Heap& heap, Rooted<Value>& target, Rooted<Value>& handler) {
    HeapObjectHeader* raw =
        heap.allocate(sizeof(ProxyHeader) - sizeof(HeapObjectHeader), Tag::Object);
    auto* proxy = reinterpret_cast<ProxyHeader*>(raw);
    proxy->header.flags = kFlags;
    proxy->target = target.get();
    proxy->handler = handler.get();
    // 10.5.14 ProxyCreate: [[Call]] is installed on the proxy if and only if
    // the target is callable AT CREATION. Recorded rather than re-derived,
    // because the target is gone after revocation and `typeof` still has to
    // answer.
    proxy->callable =
        Value::fromBool(target.get().isObject() &&
                        target.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function);
    return proxy;
}

namespace runtime {

namespace {

// 7.3.11 GetMethod(handler, name): an ordinary read, then `undefined` for
// null/undefined and a TypeError for anything else that is not callable.
// Undefined out means "no trap", which is every internal method's forward
// case. The read itself can throw (an accessor on the handler), so the caller
// tests the pending cell.
Value trapOf(Rooted<Value>& handlerRoot, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    Value found = Value(bronze_elem_get(handlerRoot.get().rawBits(), key.get().rawBits()));
    if (rtExceptionPending()) return Value::fromUndefined();
    if (found.isUndefined() || found.isNull()) return Value::fromUndefined();
    if (!rtIsCallableValue(found)) {
        rtThrowTypeError(std::string("'") + name + "' trap on proxy is not a function");
        return Value::fromUndefined();
    }
    return found;
}

}  // namespace

// A proxy's own keys, as the target answers them. `bronze_object_keys` reports
// own ENUMERABLE string keys and 10.5.11 wants every own key, so this asks the
// broader question the own-key walk already knows how to answer for the kinds
// a target can be.
Value rtProxyTargetOwnKeys(Rooted<Value>& targetRoot) {
    Value t = targetRoot.get();
    if (t.isObject() && t.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        // The vector of PropertyKeys is safe where a vector of VALUES would not
        // be: a PropertyKey names an arena-interned shape string, which is
        // immortal and never moves, and each key is copied into the heap
        // straight into the rooted result one at a time.
        const std::vector<PropertyKey> names =
            rtOwnKeysOrdered(t.asObject<ObjectHeader>(), /*enumerableOnly=*/false);
        Rooted<Value> out{Value(bronze_create_array(0))};
        uint32_t at = 0;
        for (PropertyKey name : names) {
            Rooted<Value> key{name.isSymbol() ? name.toValue() : rtCopyKeyToHeap(name.string())};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get();
    }
    // Every other target kind: the enumerable string keys the own-key walk
    // gives, which is the complete answer for an array (its indices and named
    // properties) and for a collection, and which is what every caller of this
    // in bronze — spread, `Object.keys`, `for-in` — asked for anyway.
    return Value(bronze_object_keys(targetRoot.get().rawBits()));
}

bool rtIsCallableValue(Value v) {
    if (!v.isObject()) return false;
    const uint16_t kind = v.asObject<HeapObjectHeader>()->flags;
    if (kind == HeapKind::Function) return true;
    if (kind != ProxyHeader::kFlags) return false;
    return v.asObject<ProxyHeader>()->callable.isBool() &&
           v.asObject<ProxyHeader>()->callable.asBool();
}

bool rtProxyRefuseIfRevoked(Value proxyVal, const char* operation) {
    if (!proxyVal.asObject<ProxyHeader>()->revoked()) return false;
    // 10.5.1 step 2 and its siblings: every internal method of a revoked proxy
    // throws a TypeError, and the language names the error rather than bronze
    // refusing — so a program that revokes defensively can catch it.
    rtThrowTypeError(std::string("Cannot perform '") + operation +
                     "' on a proxy that has been revoked");
    return true;
}

namespace {

// The three pieces every internal method opens with, in one place so that no
// arm can forget the revoked check or root one of the two halves by value.
// False means a TypeError is pending.
bool openProxy(Value proxyVal, const char* operation, Rooted<Value>& targetRoot,
               Rooted<Value>& handlerRoot) {
    if (rtProxyRefuseIfRevoked(proxyVal, operation)) return false;
    targetRoot.set(proxyVal.asObject<ProxyHeader>()->target);
    handlerRoot.set(proxyVal.asObject<ProxyHeader>()->handler);
    return true;
}

// 28.2.2.1.1, the revoker: nulls both slots and answers undefined. Idempotent
// — step 1 returns early when the pair is already broken — which is what makes
// the documented "revoke defensively" idiom safe. The proxy it closes over is
// its bound `this`, which is how the pair is kept without a captured
// environment.
uint64_t proxyRevoke(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!self.isObject() || self.asObject<HeapObjectHeader>()->flags != ProxyHeader::kFlags) {
        fatal("internal: a Proxy revoker invoked on a receiver that is not its proxy");
    }
    auto* proxy = self.asObject<ProxyHeader>();
    proxy->target = Value::fromNull();
    proxy->handler = Value::fromNull();
    return Value::fromUndefined().rawBits();
}

// The two operands 28.2.1.1 step 1 and 28.2.2.1 step 1 both check.
bool requireProxyOperands(uint32_t argc, const uint64_t* argv) {
    if (argc < 2 || !Value(argv[0]).isObject() || !Value(argv[1]).isObject()) {
        rtThrowTypeError("Cannot create proxy with a non-object as target or handler");
        return false;
    }
    return true;
}

}  // namespace

uint64_t rtProxyConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (!requireProxyOperands(argc, argv)) return Value::fromUndefined().rawBits();
    Rooted<Value> targetRoot{Value(argv[0])};
    Rooted<Value> handlerRoot{Value(argv[1])};
    ProxyHeader* proxy = ProxyHeader::create(rtHeap(), targetRoot, handlerRoot);
    return Value::fromObject(reinterpret_cast<HeapObjectHeader*>(proxy)).rawBits();
}

uint64_t rtProxyRevocable(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (!requireProxyOperands(argc, argv)) return Value::fromUndefined().rawBits();
    Rooted<Value> targetRoot{Value(argv[0])};
    Rooted<Value> handlerRoot{Value(argv[1])};
    Rooted<Value> proxy{Value::fromObject(
        reinterpret_cast<HeapObjectHeader*>(ProxyHeader::create(rtHeap(), targetRoot, handlerRoot)))};
    // The revoker is `proxyRevoke` BOUND to the proxy (step 3 stores the proxy
    // in the revoker's [[RevocableProxy]] slot; a bound receiver is the same
    // arrangement with machinery bronze already has). It is therefore callable
    // with no receiver of its own, which is what `const { revoke } = pair;
    // revoke()` needs.
    Rooted<Value> raw{rtNativeFunction(proxyRevoke, 0)};
    uint64_t boundThis = proxy.get().rawBits();
    Rooted<Value> revoke{Value(rtFunctionBindBuiltin(0, raw.get().rawBits(), 1, &boundThis))};

    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> proxyKey{rtMakeString("proxy")};
    bronze_elem_set(out.get().rawBits(), proxyKey.get().rawBits(), proxy.get().rawBits(),
                    /*strict=*/false);
    Rooted<Value> revokeKey{rtMakeString("revoke")};
    bronze_elem_set(out.get().rawBits(), revokeKey.get().rawBits(), revoke.get().rawBits(),
                    /*strict=*/false);
    return out.get().rawBits();
}

Value rtProxyGet(Value proxyVal, Value keyVal, Value receiver) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> receiverRoot{receiver};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "get", targetRoot, handlerRoot)) return Value::fromUndefined();

    Value trap = trapOf(handlerRoot, "get");
    if (rtExceptionPending()) return Value::fromUndefined();
    if (trap.isUndefined()) {
        // 10.5.8 step 6: no trap means the target's own [[Get]], receiver and
        // all. The receiver nuance is dropped deliberately: the target answers
        // as itself, which only a getter that inspects `this` can observe.
        Value forwarded =
            Value(bronze_elem_get(targetRoot.get().rawBits(), keyRoot.get().rawBits()));
        if (rtExceptionPending()) return forwarded;
        return rtProxyAdaptArrayMember(targetRoot, keyRoot, forwarded);
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[3] = {targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                              receiverRoot.get().rawBits()};
    Rooted<Value> result{Value(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                  handlerRoot.get().rawBits(), 3, args))};
    if (rtExceptionPending()) return Value::fromUndefined();
    rtProxyCheckGet(targetRoot, keyRoot, result);
    if (rtExceptionPending()) return Value::fromUndefined();
    return result.get();
}

void rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict, Value receiver) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> valRoot{val};
    Rooted<Value> receiverRoot{receiver};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "set", targetRoot, handlerRoot)) return;

    Value trap = trapOf(handlerRoot, "set");
    if (rtExceptionPending()) return;
    if (trap.isUndefined()) {
        bronze_elem_set(targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                        valRoot.get().rawBits(), strict);
        return;
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[4] = {targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                              valRoot.get().rawBits(), receiverRoot.get().rawBits()};
    const uint64_t result = bronze_dynamic_call(trapRoot.get().rawBits(),
                                                handlerRoot.get().rawBits(), 4, args);
    if (rtExceptionPending()) return;
    // 13.15.2 via 10.5.9 step 6: a trap that answers false refused the write,
    // and strict code turns that refusal into a TypeError.
    if (!bronze_truthy(result)) {
        if (strict) rtThrowTypeError("'set' on proxy: trap returned falsish");
        // Step 9 runs only after a SUCCESSFUL write: a refusal changed nothing
        // and so can contradict nothing.
        return;
    }
    rtProxyCheckSet(targetRoot, keyRoot, valRoot);
}

bool rtProxyHas(Value proxyVal, Value keyVal) {
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "has", targetRoot, handlerRoot)) return false;

    Value trap = trapOf(handlerRoot, "has");
    if (rtExceptionPending()) return false;
    if (trap.isUndefined()) {
        return bronze_has_property(keyRoot.get().rawBits(), targetRoot.get().rawBits());
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[2] = {targetRoot.get().rawBits(), keyRoot.get().rawBits()};
    const bool answer = bronze_truthy(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                          handlerRoot.get().rawBits(), 2, args));
    if (rtExceptionPending()) return false;
    rtProxyCheckHas(targetRoot, keyRoot, answer);
    return answer;
}

bool rtProxyDelete(Value proxyVal, Value keyVal, bool strict) {
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "deleteProperty", targetRoot, handlerRoot)) return true;

    Value trap = trapOf(handlerRoot, "deleteProperty");
    if (rtExceptionPending()) return true;
    if (trap.isUndefined()) {
        return bronze_elem_delete(targetRoot.get().rawBits(), keyRoot.get().rawBits(), strict);
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[2] = {targetRoot.get().rawBits(), keyRoot.get().rawBits()};
    const uint64_t result = bronze_dynamic_call(trapRoot.get().rawBits(),
                                                handlerRoot.get().rawBits(), 2, args);
    if (rtExceptionPending()) return true;
    const bool ok = bronze_truthy(result);
    // 13.5.1.2 step 5.b, the same rule an ordinary non-configurable delete
    // takes: false is quiet in sloppy code and a TypeError in strict.
    if (!ok && strict) {
        rtThrowTypeError("'deleteProperty' on proxy: trap returned falsish");
    }
    if (ok) rtProxyCheckDelete(targetRoot, keyRoot, ok);
    return ok;
}

Value rtProxyOwnKeys(Value proxyVal) {
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    // BEFORE the result array is allocated: `proxyVal` arrives by value, and
    // the first allocation in this function moves the proxy out from under it.
    if (!openProxy(proxyVal, "ownKeys", targetRoot, handlerRoot)) {
        return Value(bronze_create_array(0));
    }
    Rooted<Value> out{Value(bronze_create_array(0))};

    Value trap = trapOf(handlerRoot, "ownKeys");
    if (rtExceptionPending()) return out.get();
    if (trap.isUndefined()) return rtProxyTargetOwnKeys(targetRoot);

    Rooted<Value> trapRoot{trap};
    const uint64_t args[1] = {targetRoot.get().rawBits()};
    Rooted<Value> listRoot{Value(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                     handlerRoot.get().rawBits(), 1, args))};
    if (rtExceptionPending()) return out.get();
    // 7.3.18 CreateListFromArrayLike with the String/Symbol element filter of
    // 10.5.11 step 6: anything else in the list is the TypeError that step
    // names, not a key silently dropped.
    if (!listRoot.get().isObject()) {
        rtThrowTypeError("'ownKeys' on proxy: trap result is not a list of property keys");
        return out.get();
    }
    Rooted<Value> lengthKey{rtMakeString("length")};
    const double lenNum =
        rtToNumber(Value(bronze_elem_get(listRoot.get().rawBits(), lengthKey.get().rawBits())));
    if (rtExceptionPending()) return out.get();
    const auto len = static_cast<uint32_t>(lenNum < 0 ? 0 : lenNum);
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> idx{Value::fromDouble(i)};
        Rooted<Value> elem{
            Value(bronze_elem_get(listRoot.get().rawBits(), idx.get().rawBits()))};
        if (rtExceptionPending()) return out.get();
        if (!elem.get().isString() && !elem.get().isSymbol()) {
            rtThrowTypeError("'ownKeys' on proxy: trap result contains a value that is not a "
                             "property key");
            return out.get();
        }
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, elem);
    }
    rtProxyCheckOwnKeys(targetRoot, out);
    if (rtExceptionPending()) return Value(bronze_create_array(0));
    return out.get();
}

bool rtProxyGetOwnProperty(Value proxyVal, Value keyVal, OwnPropertyDetail& out) {
    out = OwnPropertyDetail{};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "getOwnPropertyDescriptor", targetRoot, handlerRoot)) return false;

    Value trap = trapOf(handlerRoot, "getOwnPropertyDescriptor");
    if (rtExceptionPending()) return false;
    if (trap.isUndefined()) {
        // Forwarded: the target's own-property question, asked the way
        // `hasOwnProperty` asks it so the two cannot disagree.
        Rooted<Value> t{targetRoot.get()};
        return rtOwnPropertyOf(t, keyRoot.get(), out);
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[2] = {targetRoot.get().rawBits(), keyRoot.get().rawBits()};
    Rooted<Value> desc{Value(bronze_dynamic_call(trapRoot.get().rawBits(),
                                                 handlerRoot.get().rawBits(), 2, args))};
    if (rtExceptionPending()) return false;
    // 10.5.5 step 6: anything that is neither an object nor undefined is a
    // TypeError. Step 7's `undefined` still goes through the invariant check,
    // which is what makes "the target has a non-configurable `k`" impossible to
    // hide behind an absent descriptor.
    if (!desc.get().isObject() && !desc.get().isUndefined()) {
        rtThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap returned neither an object "
                         "nor undefined");
        return false;
    }
    rtProxyCheckGetOwnProperty(targetRoot, keyRoot, desc);
    if (rtExceptionPending()) return false;
    if (desc.get().isUndefined()) return false;
    // What every caller in bronze reads back off the descriptor. The
    // attributes are the trap's own, completed the way 6.2.6.6 completes them:
    // a field the trap left off is false, which is what makes
    // `propertyIsEnumerable` on a proxy answer about the descriptor rather than
    // about the target.
    Rooted<Value> enumKey{rtMakeString("enumerable")};
    out.enumerable =
        bronze_truthy(bronze_elem_get(desc.get().rawBits(), enumKey.get().rawBits()));
    return !rtExceptionPending();
}

Value rtProxyGetPrototypeOf(Value proxyVal) {
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "getPrototypeOf", targetRoot, handlerRoot)) {
        return Value::fromUndefined();
    }
    Value trap = trapOf(handlerRoot, "getPrototypeOf");
    if (rtExceptionPending()) return Value::fromUndefined();
    if (trap.isUndefined()) {
        const uint64_t args[1] = {targetRoot.get().rawBits()};
        return Value(objectGetPrototypeOf(0, 0, 1, args));
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[1] = {targetRoot.get().rawBits()};
    Value result = Value(bronze_dynamic_call(trapRoot.get().rawBits(),
                                             handlerRoot.get().rawBits(), 1, args));
    if (rtExceptionPending()) return Value::fromUndefined();
    // 10.5.1 step 6: a prototype is an object or null, and nothing else.
    if (!result.isObject() && !result.isNull()) {
        rtThrowTypeError("'getPrototypeOf' on proxy: trap returned neither an object nor null");
        return Value::fromUndefined();
    }
    Rooted<Value> resultRoot{result};
    rtProxyCheckPrototype(targetRoot, resultRoot);
    if (rtExceptionPending()) return Value::fromUndefined();
    return resultRoot.get();
}

namespace {

// 7.3.17 CreateArrayFromList, which both [[Call]] and [[Construct]] hand the
// trap. A real Array, not an `arguments` object: `Array.isArray(args)` inside
// an `apply` trap is true, and a trap that called `.map` on what it received
// would otherwise fail on the first line.
Value argumentsArray(uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arr{Value(bronze_create_array(argc))};
    for (uint32_t i = 0; i < argc; ++i) {
        Rooted<Value> elem{args[i]};
        arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, elem);
    }
    return arr.get();
}

}  // namespace

uint64_t rtProxyCall(Value proxyVal, Value thisArg, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> thisRoot{thisArg};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "apply", targetRoot, handlerRoot)) {
        return Value::fromUndefined().rawBits();
    }
    // Copied out of `argv` before anything can allocate: the block belongs to
    // the caller's frame, and the trap lookup below is a read that can collect.
    RootedBlock incoming(argc);
    for (uint32_t i = 0; i < argc; ++i) incoming.set(i, Value(argv[i]));

    Value trap = trapOf(handlerRoot, "apply");
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (trap.isUndefined()) {
        // 10.5.12 step 4: the call forwards unchanged — same `this`, same
        // arguments. `thisArgument` is whatever the call site passed, which for
        // `p.m()` is the PROXY, and that is the answer the specification gives.
        return bronze_dynamic_call(targetRoot.get().rawBits(), thisRoot.get().rawBits(),
                                   incoming.count(), incoming.data());
    }
    Rooted<Value> trapRoot{trap};
    Rooted<Value> argArray{argumentsArray(incoming.count(), incoming.data())};
    const uint64_t args[3] = {targetRoot.get().rawBits(), thisRoot.get().rawBits(),
                              argArray.get().rawBits()};
    return bronze_dynamic_call(trapRoot.get().rawBits(), handlerRoot.get().rawBits(), 3, args);
}

uint64_t rtProxyConstruct(Value proxyVal, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> targetRoot;
    Rooted<Value> handlerRoot;
    if (!openProxy(proxyVal, "construct", targetRoot, handlerRoot)) {
        return Value::fromUndefined().rawBits();
    }
    if (!proxyRoot.get().asObject<ProxyHeader>()->callable.asBool()) {
        return rtThrowTypeError("proxy is not a constructor").rawBits();
    }
    RootedBlock incoming(argc);
    for (uint32_t i = 0; i < argc; ++i) incoming.set(i, Value(argv[i]));

    Value trap = trapOf(handlerRoot, "construct");
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (trap.isUndefined()) {
        // 10.5.13 step 5 constructs the TARGET with the PROXY as newTarget.
        // bronze threads newTarget through a scope rather than a parameter, and
        // the one thing the difference reaches — the instance's prototype,
        // which comes from Get(newTarget, "prototype") — forwards to the
        // target's `prototype` whenever there is no `get` trap. A `get` trap
        // that answers `prototype` differently is therefore not honoured here;
        // it is the one part of [[Construct]] this does not reproduce.
        return bronze_construct(targetRoot.get().rawBits(), incoming.count(), incoming.data());
    }
    Rooted<Value> trapRoot{trap};
    Rooted<Value> argArray{argumentsArray(incoming.count(), incoming.data())};
    const uint64_t args[3] = {targetRoot.get().rawBits(), argArray.get().rawBits(),
                              proxyRoot.get().rawBits()};
    Value result = Value(
        bronze_dynamic_call(trapRoot.get().rawBits(), handlerRoot.get().rawBits(), 3, args));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    // 10.5.13 step 9: a non-object return is a TypeError, not a value quietly
    // replaced by the instance the way an ordinary constructor's would be.
    if (!result.isObject()) {
        return rtThrowTypeError("'construct' on proxy: trap returned a non-object").rawBits();
    }
    return result.rawBits();
}

}  // namespace runtime
}  // namespace bronze
