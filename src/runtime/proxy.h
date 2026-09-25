#pragma once

#include <string>

#include "runtime/dictionary.h"
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
// handler with no trap for an operation forwards it to the target. All
// thirteen internal methods are here: the nine property-shaped ones in
// proxy.cpp, and [[GetPrototypeOf]]'s three siblings, [[DefineOwnProperty]]
// and the integrity operations built over them in proxy_reflect.cpp.
//
// REVOCATION (28.2.2.1) is a state, not a kind: the revoker nulls both fields,
// and every internal method below begins with the check 10.5.1 step 2 and its
// siblings name. `typeof` is the one operation that does not throw on a
// revoked proxy, which is why callability is recorded at creation and survives
// revocation.
struct ProxyHeader {
    HeapObjectHeader header;
    HeapValue target;    // an object; NULL once revoked
    HeapValue handler;   // an object; NULL once revoked
    // 10.5.14: [[Call]] and [[Construct]] are present on the proxy exactly
    // when the TARGET had them at creation, so both are decided once and read
    // afterwards — including after revocation, when the target is gone but
    // `typeof p` must still answer "function". Two facts, not one: an arrow
    // or a method is callable without being a constructor, and `new` on a
    // proxy over one must be the TypeError of a missing [[Construct]] rather
    // than a `construct` trap call (10.5.13 exists only when this is true).
    HeapValue callable;      // boolean
    HeapValue constructible; // boolean

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

// ECMA-262 7.2.4 IsConstructor, over the same value model: a function object
// with [[Construct]] (10.2.2 gives an arrow, a method and the generator forms
// none; 21.2.1.1 gives `BigInt` none), or a Proxy whose target was a
// constructor when it was created.
bool rtIsConstructorValue(Value v);

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
// TypeError, false otherwise is a quiet refusal — and it is also RETURNED, for
// the one caller that spends it differently: 10.1.9.2 step 2 of an ordinary
// object whose chain holds the proxy, which reports the refusal to its own
// strict caller rather than throwing here. `receiver` is step 7's fourth trap
// argument, on the same terms as [[Get]]'s above.
bool rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict, Value receiver);
inline bool rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict) {
    return rtProxySet(proxyVal, keyVal, val, strict, proxyVal);
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

// [[GetOwnProperty]] (10.5.5). False means absent; a trap's throw propagates.
// The attributes come back in `out`, which
// is what lets a proxy stand where an ordinary object stands in the invariant
// checks below: a proxy over a proxy asks its own trap and reports the
// descriptor the trap built, rather than reporting only that something is
// there.
bool rtProxyGetOwnProperty(Value proxyVal, Value keyVal, struct OwnPropertyDetail& out);

// The same, as far as the TRAP takes it: `Forwarded` means the handler has no
// `getOwnPropertyDescriptor` trap and the target has not been asked, so the
// caller asks it in whichever spelling it wants — the attribute switch above,
// or `Object.getOwnPropertyDescriptor`'s object, which is the only spelling
// that describes every target kind. `Present` fills `out` with the trap's
// descriptor COMPLETED (6.2.6.6), value included.
enum class ProxyOwnProperty { Forwarded, Absent, Present };
ProxyOwnProperty rtProxyGetOwnPropertyTrapped(Value proxyVal, Value keyVal,
                                              struct OwnPropertyDetail& out);

// [[GetPrototypeOf]] (10.5.1): the `getPrototypeOf` trap, or the target's.
Value rtProxyGetPrototypeOf(Value proxyVal);

// ---- proxy_reflect.cpp -----------------------------------------------------
//
// The internal methods whose forward is an `Object` member rather than a
// property read, and the two integrity operations (7.3.14, 7.3.15) that are
// defined over four of them. Every one is a GC point and a throw point on the
// same terms as the family above.

// [[GetOwnProperty]] (10.5.5) as the OBJECT 6.2.6.4 FromPropertyDescriptor
// builds from it, or `undefined`: what `Object.getOwnPropertyDescriptor` and
// `Reflect.getOwnPropertyDescriptor` answer for a proxy.
Value rtProxyGetOwnPropertyDescriptor(Value proxyVal, Value keyVal);

// [[DefineOwnProperty]] (10.5.6). `descVal` is the descriptor as an OBJECT —
// already decoded once by the caller and rebuilt through FromPropertyDescriptor
// (step 9), so the trap sees exactly the fields the descriptor has and nothing
// the program's own object carried besides. `throwOnRefusal` is the
// `Object.defineProperty` / `Reflect.defineProperty` split: a trap that
// answers false is the TypeError for the first and `false` for the second.
bool rtProxyDefineOwnProperty(Value proxyVal, Value keyVal, Value descVal, bool throwOnRefusal);

// [[SetPrototypeOf]] (10.5.2), [[IsExtensible]] (10.5.3) and
// [[PreventExtensions]] (10.5.4): the boolean each internal method answers.
// A refusal is the boolean; a contradiction of the target is the TypeError.
bool rtProxySetPrototypeOf(Value proxyVal, Value protoVal);
bool rtProxyIsExtensible(Value proxyVal);
bool rtProxyPreventExtensions(Value proxyVal);

// 7.3.14 SetIntegrityLevel and 7.3.15 TestIntegrityLevel over a proxy: the
// generic algorithms, spelled over the internal methods above, which is what
// makes `Object.freeze(proxy)` reach `preventExtensions`, `ownKeys`,
// `getOwnPropertyDescriptor` and `defineProperty` in 7.3.14's order.
// `IntegrityLevel::Open` is `preventExtensions` alone (20.1.2.19).
bool rtProxySetIntegrityLevel(Value proxyVal, IntegrityLevel level);
bool rtProxyTestIntegrityLevel(Value proxyVal, bool frozen);

// 7.2.2 IsArray over the whole value model: an Array exotic object, or a
// proxy — however deep — whose ultimate target is one. A revoked proxy on the
// way is step 3.b's TypeError, thrown. It lives
// beside the proxy because the proxy is the only reason the question is not
// a kind test; `Array.isArray` and `Object.prototype.toString` ask it.
bool rtIsArray(Value v);

// 7.3.23 EnumerableOwnProperties over a proxy, for `Object.values` and
// `Object.entries`: [[OwnPropertyKeys]] once, then PER KEY a
// [[GetOwnProperty]] and — for an enumerable one — a [[Get]], which is the
// interleaving a handler observes and `Object.keys` followed by reads is not.
Value rtProxyEnumerableOwn(Value proxyVal, bool wantEntries);

// [[Call]] (10.5.12): the `apply` trap called as `trap(target, thisArg,
// argsArray)` — the arguments as a real Array, which is CreateArrayFromList in
// step 6 and not an `arguments` object — or the forwarded call, `this` and all.
uint64_t rtProxyCall(Value proxyVal, Value thisArg, uint32_t argc, const uint64_t* argv);

// [[Construct]] (10.5.13): the `construct` trap called as `trap(target,
// argsArray, newTarget)`, whose non-object return is step 9's TypeError — or
// the forwarded construction, `Construct(target, args, newTarget)`. The
// newTarget is the proxy itself for `new p()`, the third argument of
// `Reflect.construct`, and the active new.target for a `super()` that
// reaches a proxy base. `argv` must be rooted by the caller.
uint64_t rtProxyConstruct(Value proxyVal, uint32_t argc, const uint64_t* argv, Value newTarget);

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
// takes roots. Each throws its TypeError on a violation and otherwise
// returns, exactly as a trap call does.
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
// runs 6.2.6's IsCompatiblePropertyDescriptor. `reported` comes back holding
// the trap's descriptor after ToPropertyDescriptor and
// CompletePropertyDescriptor (steps 12-13): decoded ONCE, here, because each
// field read is a [[Get]] on the trap's object and may run a getter.
void rtProxyCheckGetOwnProperty(Rooted<Value>& target, Rooted<Value>& key,
                                Rooted<Value>& desc, OwnPropertyDetail& reported);

// The fields of a decoded descriptor that the [[DefineOwnProperty]] check
// reads, spelled without a dependency on the descriptor seam's own struct:
// which fields are PRESENT, the three booleans, and the payloads (rooted by
// the caller).
struct DecodedDescriptorView {
    bool hasValue = false, hasWritable = false, hasEnumerable = false, hasConfigurable = false;
    bool hasGet = false, hasSet = false;
    bool writable = false, enumerable = false, configurable = false;
    Rooted<Value>* value = nullptr;
    Rooted<Value>* getter = nullptr;
    Rooted<Value>* setter = nullptr;
};

// 10.5.6 steps 11-17: the descriptor a `defineProperty` trap accepted against
// the target's own property — 6.2.6.7 IsCompatiblePropertyDescriptor over the
// PARTIAL descriptor (a field the descriptor lacks constrains nothing), and the
// two rules about manufacturing non-configurability and non-writability.
void rtProxyCheckDefineProperty(Rooted<Value>& target, Rooted<Value>& key,
                                const DecodedDescriptorView& desc);

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
