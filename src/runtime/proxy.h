#pragma once

#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_property.h"
#include "runtime/value.h"

namespace bronze {

// A Proxy exotic object (ECMA-262 10.5).
//
// A trap is found the way 10.5 finds one — GetMethod(handler, name), an
// ordinary read followed by a callable check — so ANY object is a legal
// handler (10.5.14 requires only an object), an inherited trap is found, and a
// handler with no trap for an operation forwards it to the target. There is no
// construction-time gate over the trap names: the operations whose traps
// bronze has NOT built refuse BY NAME at the operation instead
// (`Object.defineProperty`, `Object.setPrototypeOf`, `Object.freeze` and the
// extensibility pair on a proxy), which is the same guarantee — no operation
// silently bypasses a handler — placed where it can name what was asked for.
//
// REVOCATION (28.2.2.1) is a state, not a kind: the revoker nulls both fields,
// and every internal method below begins with the check 10.5.1 step 2 and its
// siblings name. `typeof` is the one operation that does not throw on a
// revoked proxy, which is why callability is recorded at creation and survives
// revocation.
struct ProxyHeader {
    HeapObjectHeader header;
    Value target;    // an object; NULL once revoked
    Value handler;   // an object; NULL once revoked
    // 10.5.14: [[Call]] and [[Construct]] are present on the proxy exactly
    // when the TARGET had them at creation, so this is decided once and read
    // afterwards — including after revocation, when the target is gone but
    // `typeof p` must still answer "function".
    Value callable;  // boolean

    static constexpr uint16_t kFlags = HeapKind::Proxy;

    // Rooted operands, not Values: the allocation inside can move both, and
    // a by-value copy would store the from-space address it was handed.
    static ProxyHeader* create(Heap& heap, Rooted<Value>& target, Rooted<Value>& handler);

    bool revoked() const noexcept { return handler.isNull(); }
};

namespace runtime {

// 28.2.1.1 Proxy(target, handler).
uint64_t rtProxyConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

// 28.2.2.1 Proxy.revocable(target, handler): `{ proxy, revoke }`.
uint64_t rtProxyRevocable(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

// ECMA-262 7.2.3 IsCallable, over the whole value model: a function object, or
// a Proxy whose target was callable when it was created. It lives beside the
// proxy rather than beside the function because the proxy is the only reason
// the question is not `flags == HeapKind::Function`.
bool rtIsCallableValue(Value v);

// [[Get]] (10.5.8): the `get` trap if the handler has one, else the target's
// own read through the ordinary funnel.
//
// `receiver` is step 8's third trap argument — what `this` is bound to if the
// read finds an accessor. For an ordinary `p.k` it IS the proxy, which is what
// the two-argument spelling means; `Reflect.get(p, k, other)` is the one caller
// that has a different one, and passing it through is the whole of what the
// third parameter of 28.1.6 does.
Value rtProxyGet(Value proxyVal, Value keyVal, Value receiver);
inline Value rtProxyGet(Value proxyVal, Value keyVal) {
    return rtProxyGet(proxyVal, keyVal, proxyVal);
}

// [[Set]] (10.5.9): the `set` trap or the forwarded write. The trap's
// boolean result is read the way 13.15.2 reads it: false under strict is a
// TypeError, false otherwise is a quiet refusal. `receiver` is step 7's fourth
// trap argument, on the same terms as [[Get]]'s above.
void rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict, Value receiver);
inline void rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict) {
    rtProxySet(proxyVal, keyVal, val, strict, proxyVal);
}

// [[HasProperty]] (10.5.7): the `has` trap or the forwarded `in`.
bool rtProxyHas(Value proxyVal, Value keyVal);

// [[Delete]] (10.5.10). Answers what `delete` answers; `strict` turns a false
// into 13.5.1.2 step 5.b's TypeError, exactly as an ordinary delete does.
bool rtProxyDelete(Value proxyVal, Value keyVal, bool strict);

// [[OwnPropertyKeys]] (10.5.11): the `ownKeys` trap's list through 7.3.18
// CreateListFromArrayLike, or the target's own keys. Order is the trap's own,
// which is what 10.5.11 gives — a trap may reorder, and only the invariant
// checks below constrain what it may omit.
//
// An ARRAY, not a vector of Values, because every caller then does something
// that allocates FOR EACH KEY — a descriptor trap, a [[Get]], a write into
// the result — and a std::vector the collector cannot see would be holding
// from-space strings by the second iteration. The caller roots the array and
// re-reads the element it is on.
Value rtProxyOwnKeys(Value proxyVal);

// The TARGET's own keys — what the forward arm of [[OwnPropertyKeys]] above
// hands back when a handler has no `ownKeys` trap. Public because 10.5.11's
// invariant check needs the same list, and a check that asked a different
// question than the forward it guards could refuse a key list the forward
// would have produced.
Value rtProxyTargetOwnKeys(Rooted<Value>& targetRoot);

// [[GetOwnProperty]] (10.5.5). False means absent (or that the trap threw — the
// caller must test the pending cell). The attributes come back in `out`, which
// is what lets a proxy stand where an ordinary object stands in the invariant
// checks below: a proxy over a proxy asks its own trap and reports the
// descriptor the trap built, rather than reporting only that something is
// there.
bool rtProxyGetOwnProperty(Value proxyVal, Value keyVal, struct OwnPropertyDetail& out);

// [[GetPrototypeOf]] (10.5.1): the `getPrototypeOf` trap, or the target's.
Value rtProxyGetPrototypeOf(Value proxyVal);

// [[Call]] (10.5.12): the `apply` trap called as `trap(target, thisArg,
// argsArray)` — the arguments as a real Array, which is CreateArrayFromList in
// step 6 and not an `arguments` object — or the forwarded call, `this` and all.
uint64_t rtProxyCall(Value proxyVal, Value thisArg, uint32_t argc, const uint64_t* argv);

// [[Construct]] (10.5.13): the `construct` trap called as `trap(target,
// argsArray, newTarget)`, whose non-object return is step 9's TypeError — or
// the forwarded construction.
uint64_t rtProxyConstruct(Value proxyVal, uint32_t argc, const uint64_t* argv);

// The check every one of the above begins with, exposed for the callers that
// reach a proxy without going through one of them. True means a TypeError is
// now pending and the caller must stop.
bool rtProxyRefuseIfRevoked(Value proxyVal, const char* operation);

// ---- the essential invariants (proxy_invariant.cpp) ------------------------
//
// 10.5's internal methods do not merely CALL a trap: each one then checks the
// answer against the target, and throws a TypeError when the two contradict.
// The checks are what make a proxy safe to hand out — a non-configurable
// non-writable property cannot be made to read two different values, an
// unextensible target cannot be made to appear to gain a key — so they are the
// operations' second half and not a nicety.
//
// Every one of them is a THROW POINT and (through the target's own
// [[GetOwnProperty]], which may itself be a trap) a GC POINT, which is why each
// takes roots. Each leaves a pending TypeError and answers nothing; the caller
// tests the pending cell exactly as it does after a trap call.
//
// The whole family is on the TRAPPED path only. A handler with no trap for an
// operation forwards to the target, and a forward cannot contradict the target
// by construction — so a proxy used as a plain forwarder pays for none of this.

// 10.5.8 step 10 and 10.5.9 step 9: what a `get` may answer, and what a `set`
// may claim to have written, for a non-configurable target property.
void rtProxyCheckGet(Rooted<Value>& target, Rooted<Value>& key, Rooted<Value>& trapResult);
void rtProxyCheckSet(Rooted<Value>& target, Rooted<Value>& key, Rooted<Value>& value);

// 10.5.7 step 9 and 10.5.10 steps 11-13: a `has` that denies a
// non-configurable property (or any property of a non-extensible target), and a
// `deleteProperty` that claims to have removed one.
void rtProxyCheckHas(Rooted<Value>& target, Rooted<Value>& key, bool trapAnswer);
void rtProxyCheckDelete(Rooted<Value>& target, Rooted<Value>& key, bool trapAnswer);

// 10.5.5 steps 9-17, over the descriptor object the trap returned (or a
// `Rooted` holding undefined for the absent answer). This is the one check that
// runs 6.2.6's IsCompatiblePropertyDescriptor.
void rtProxyCheckGetOwnProperty(Rooted<Value>& target, Rooted<Value>& key,
                                Rooted<Value>& desc);

// 10.5.11 steps 9-23: the trap's key list against the target's non-configurable
// keys, and — for a non-extensible target — against ALL of them, in both
// directions. `keys` is the array 10.5.11 built from the trap's result.
void rtProxyCheckOwnKeys(Rooted<Value>& target, Rooted<Value>& keys);

// 10.5.1 steps 7-9: a `getPrototypeOf` trap may answer freely for an
// extensible target and must answer the truth for one that is not.
void rtProxyCheckPrototype(Rooted<Value>& target, Rooted<Value>& trapResult);

// What a forwarded read of an array MEMBER hands back, for a proxy whose
// target is an array (proxy_array.cpp). Public because [[Get]] is the only
// caller and the two live in different files.
Value rtProxyAdaptArrayMember(Rooted<Value>& targetRoot, Rooted<Value>& keyRoot,
                              Value forwarded);

}  // namespace runtime
}  // namespace bronze
