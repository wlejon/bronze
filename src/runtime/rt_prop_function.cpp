// Property READS on a FUNCTION receiver — a class constructor, an ordinary
// function, one of the interned intrinsic singletons. Its own translation unit
// for the reason rt_prop.cpp is split by receiver kind at all: this is one
// kind, and it is the one whose answer comes from FOUR places in a fixed order
// — the global-constructor table, the `prototype` slot, the function's own
// statics object, and a ladder of intrinsic tables ending at
// %Function.prototype% and %Object.prototype%.
//
// That order is the file's whole content and every step of it is load-bearing;
// each one carries the clause it implements. The one thing to know before
// reading it is that a function's `static` members do not live in the
// FunctionHeader. They live in a side object hanging off `properties`, with its
// own shape and its own prototype chain — the chain `extends` links to the base
// class's statics. `C.DEFAULT_UP` is therefore an own property of an object
// that is not the receiver, which is what `installStaticsCacheEntry` below has
// to say out loud to the inline cache.

#include <cstdlib>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/value.h"

namespace bronze::runtime {

// THE SEAM, runtime half. `BRONZE_NO_FN_STATICS_IC=1` stops both fills below,
// so that one binary A/Bs the whole mechanism: codegen's half of the same
// variable (llvm_prop_get.cpp, llvm_method_call.cpp) stops emitting the statics
// arms, and this stops warming entries those arms would be the only readers of.
// Gating only codegen would leave the fill running and let a statics entry
// evict a plain-object way, which is a difference between the arms that is not
// the thing being measured.
static uint32_t g_fnStaticsIcEnabled = 1u;

void fnStaticsIcReadSeam() noexcept {
    const char* env = std::getenv("BRONZE_NO_FN_STATICS_IC");
    g_fnStaticsIcEnabled = (env != nullptr && std::strcmp(env, "1") == 0) ? 0u : 1u;
}

// May a shape-keyed inline-cache entry describe this (function receiver,
// statics box) pair at all? Two fills ask it: the property READ's
// `installStaticsCacheEntry` below, and the method call's
// `latchFunctionStaticsMethodIc` (rt_method_call.cpp), which caches the same
// box's shape and slot so a call's callee lookup hits where a read's does. One
// function rather than a copy each, because every refusal here is about the
// RECEIVER and the BOX and not about what the site goes on to do with the slot
// — the two askers differ only in that.
//
// Three refusals, and each one is a correctness condition rather than a
// heuristic:
//
//   - An INTRINSIC CONSTRUCTOR is never cached. `rtGlobalConstructorMember`
//     ran ahead of the statics lookup and won for every name it knows, so a
//     shape-keyed entry could answer where the table is supposed to. The
//     collision is reachable, not theoretical: `class C { static keys = 1 }`
//     gives C's statics the shape `{keys}`, and a first `Object.keys = 9`
//     would give %Object%'s statics the same interned shape pointer — after
//     which one warm site would answer 9 where the language says the builtin.
//   - An ACCESSOR is never cached — the caller's own gate, since only it knows
//     whether the property it found is one. A `static get` sees the CLASS as
//     its receiver, not the side object the getter is stored in (which is why
//     the read below hands `getProp` the function's own slot). Generated
//     code's inline arms hold the box, so they would dispatch the getter with
//     the wrong receiver; both stay on the helper instead.
//   - A DICTIONARY shape is never cached, on the same terms as the plain-object
//     path: its slots are not shape-indexed and its shape is private to one
//     object, so an entry naming it goes stale on the next delete.
//
// A statics shape and a plain object's shape are drawn from the same arena and
// CAN be the same pointer — `rtEnsureFunctionProperties` starts the box from
// `rtPlainObjectShape()`, the very root `{}` starts from. That collision is
// harmless and is worth saying why: a shape IS a key-to-slot map and nothing
// else, so a plain receiver that matches such an entry finds the site's key at
// the same slot in ITSELF, which is the answer it should have had. What an
// entry must not do is survive into a case where the answer does NOT come from
// a key-to-slot map, and the refusals above are exactly those cases.
bool rtStaticsBoxCacheable(Value fnVal, const ObjectHeader* box) noexcept {
    if (g_fnStaticsIcEnabled == 0) return false;
    if (box == nullptr || box->shape == nullptr || box->shape->isDictionary()) return false;
    // The intrinsic-constructor gate. `rtIntrinsicConstructorName` is true for
    // exactly the receivers `rtGlobalConstructorMember` can answer for — both
    // ask whether the FunctionHeader's code pointer is one of `kCtors` — so
    // this refuses precisely the receivers whose answer is decided ahead of
    // the box, and nothing else. It has to be the receiver test and not the
    // (receiver, key) one: the table's claim travels with the receiver, and a
    // key it declines today is one another site may ask it about.
    if (rtIntrinsicConstructorName(fnVal) != nullptr) return false;
    return !censusFillsSuppressed();
}

namespace {

// Teach this site that the answer for a FUNCTION receiver is an own data
// property of the receiver's STATICS object, so the next read hits inline
// instead of walking this whole file again.
//
// What goes in the entry is the statics object's shape and slot, NOT the
// receiver's — a function has no shape of its own. Generated code holds up its
// end by loading `properties` out of the FunctionHeader and scanning way 0
// against THAT object's shape (llvm_prop_get.cpp), so the two sides describe
// the same object or neither does.
//
// Depth is 0 by construction: only an OWN property of the statics object gets
// here. A static INHERITED through `extends` is answered further down this file
// and fills nothing, because the entry has no way to say "own property of an
// ancestor of the receiver's statics object" that generated code could check.
void installStaticsCacheEntry(Value fnVal, InlineCacheSite* site, ObjectHeader* propsObj,
                              const PropertyInfo& own) {
    if (site == nullptr || own.accessor) return;
    if (!rtStaticsBoxCacheable(fnVal, propsObj)) return;
    if (InlineCache* into = site->slotForInstall(propsObj->shape, rtIcWayLimit())) {
        into->fill(propsObj->shape, own.slot, /*depth=*/0);
    }
}

}  // namespace

uint64_t rtFunctionMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                          InlineCacheSite* site) {
    // Rooted for the same reason the array branch is: the tail below walks
    // `Object.prototype`, and everything between here and there allocates.
    Rooted<Value> recv{objVal};
    // A GLOBAL CONSTRUCTOR's statics come first, ahead of the `prototype`
    // slot below. That order is the whole point: a FunctionHeader answers
    // `prototype` from a slot it creates on demand, so `Array.prototype`
    // would read as an empty object — a silent lie about an intrinsic
    // bronze does not have, and one a program could install a method on
    // that nothing would ever find.
    if (Value ctorMember; rtGlobalConstructorMember(recv.get(), keyStr, ctorMember)) {
        return ctorMember.rawBits();
    }
    // `prototype` lives in its own slot; every other own property lives in
    // the function's property object and is found through ITS prototype
    // chain, which `extends` linked to the base class's. Reading
    // `prototype` first is what keeps `call`, `bind` and `name` answered
    // or diagnosed rather than read as undefined.
    if (keyStr == "prototype") {
        if (rtIsFunctionPrototype(recv.get())) {
            return Value::fromUndefined().rawBits();
        }
        if (rtIsFunctionConstructor(recv.get())) {
            return rtFunctionPrototypeObject().rawBits();
        }
        // The guard above only covers `kCtors`. Map, Set, ArrayBuffer and
        // the nine views are interned function singletons of their own, so
        // without this they reached the on-demand slot below and
        // `Map.prototype` answered a fresh empty object — the exact lie
        // the comment above says the ordering exists to prevent, told
        // about every intrinsic that is not one of the three. Named here
        // rather than by adding `prototype` to nine more tables, because
        // the property is absent for the same one reason each time.
        const char* intrinsic = rtNoPrototypeObjectIntrinsic(recv.get());
        if (intrinsic) {
            fatal((std::string("unsupported: ") + intrinsic +
                   ".prototype is not implemented (bronze has no prototype OBJECT for this "
                   "intrinsic; its methods are answered by the property path)")
                      .c_str());
        }
        // An arrow, a method, an accessor and an async function have NO
        // `prototype` property at all (15.3.4, 15.4.4, 15.8.4 build them
        // with no CreateMethodProperty step for it), so the read is
        // `undefined` and — because the slot stays empty — nothing later
        // hands `new` an instance prototype either.
        if (!recv.get().asObject<FunctionHeader>()->hasPrototypeProperty()) {
            return Value::fromUndefined().rawBits();
        }
        rtEnsureFunctionPrototype(recv);
        return recv.get().asObject<FunctionHeader>()->prototype.rawBits();
    }
    // ROOTED for the whole branch, not just for the own-property block: the
    // box is read TWICE, once for the function's own statics here and again
    // for the ones `extends` linked in, with the entire miss ladder in
    // between. Every probe in that ladder is meant to be allocation-free
    // for a receiver that is not its own, but "meant to" is not a
    // guarantee a compiler checks, and one probe that builds an intrinsic
    // on first use is enough to relocate this box — after which the second
    // read walks a retired header and either misses a static that is there
    // or dereferences a garbage shape. Rooting it costs one shadow-stack
    // slot and makes the ladder's discipline unnecessary rather than
    // load-bearing.
    Rooted<Value> props{recv.get().asObject<FunctionHeader>()->properties};
    if (props.get().isObject()) {
        // The receiver a `static get` sees is the CLASS, not the side
        // object its statics are kept in — which is the whole reason
        // getProp takes a receiver at all.
        ObjectHeader* propsObj = props.get().asObject<ObjectHeader>();
        PropertyInfo own;
        if (propsObj->shape &&
            propsObj->shape->lookupProperty(
                PropertyKey::forString(keyHeader), own)) {
            // Before the read, because the read can run a getter and a getter
            // can collect: `propsObj` and `own` are both dead after it, and the
            // entry is about the shape they name now.
            installStaticsCacheEntry(recv.get(), site, propsObj, own);
            Rooted<Value> key(Value::fromString(keyHeader));
            return propsObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
        }
    }
    // `length` and `name`, the two own data properties 10.2.10 and 10.2.9
    // give every function object. Both are non-writable and non-enumerable,
    // and both live in the header rather than in the statics table above:
    // they are created by OrdinaryFunctionCreate before any `static` can be
    // written, and a program cannot overwrite either (rt_prop_write.cpp
    // refuses the assignment).
    //
    // They are read AFTER the statics all the same, and that order is the
    // language's: `class C { static name() {} }` DEFINES a `name` property
    // over the one 15.7.14 step 15 had just given the constructor, so the
    // method wins. An assignment could not have put anything there, so the
    // only thing this order can find first is a definition that really did
    // replace the property.
    if (const FunctionHeader* fn = recv.get().asObject<FunctionHeader>(); fn->name) {
        if (keyStr == "length") return Value::fromDouble(fn->length).rawBits();
        if (keyStr == "name") return rtKeyAsValue(fn->name).rawBits();
    } else if (keyStr == "length" || keyStr == "name") {
        // A function bronze did not compile: a native builtin, or a method
        // whose key is computed at run time. rt_members.cpp's table would
        // report the member "not implemented", which is the wrong sentence
        // now that it is — what is missing is this function's own answer.
        fatal((std::string("unsupported: `") + keyStr +
               "` of a function whose name bronze never recorded (a built-in, or a member "
               "whose key is computed at run time; a function the compiler created answers "
               "both)")
                  .c_str());
    }
    // `Symbol` is a function object so that `Symbol("tag")` names bronze
    // rather than reporting that an object is not callable, which means its
    // unimplemented members reach the FUNCTION miss path rather than a
    // namespace object's. 23.2.6.2's own data property, before the
    // Function.prototype table: a typed-array constructor really carries
    // it, so answering `undefined` would be a silent lie about a name
    // ECMA-262 defines.
    if (Value stat; rtTypedArrayStatic(recv.get(), keyStr, stat)) return stat.rawBits();
    // `Map.groupBy` (24.1.2.1), on the same terms and for the same reason:
    // the `Map` constructor is an interned function singleton with no
    // property object, so its one own member is answered from a table.
    if (Value stat; rtMapStatic(recv.get(), keyStr, stat)) return stat.rawBits();
    // `RegExp.escape` (22.2.5.2), on the same terms: `RegExp` is an interned
    // function singleton too.
    if (Value stat; rtRegExpStatic(recv.get(), keyStr, stat)) return stat.rawBits();
    rtSymbolCheckMissingMember(recv.get(), keyStr);
    // `Object` is a function object too (20.1.1), so its unimplemented-member
    // table is consulted on THIS miss path and not on the plain object one
    // below. Without this line a name 20.1.2 defines and bronze has not
    // built read `undefined` from the moment `Object` stopped being a
    // namespace.
    rtObjectCheckMissingMember(recv.get(), keyStr);
    // The same step for `Promise`, whose statics live in the properties
    // object read above — so a name 27.2.4 defines and bronze has not
    // built (`try`) reaches here having missed, and is
    // refused BY NAME rather than falling through to `undefined` the way
    // every other unknown member of a function object does.
    if (rtIsPromiseConstructor(recv.get())) rtCheckPromiseStaticMember(keyStr);
    // `constructor` for the three forms that do not inherit it from
    // %Function.prototype%: a generator function's is %GeneratorFunction%,
    // not `Function`, and the table below cannot tell them apart because
    // it is asked by KEY and never sees the receiver. Answered `undefined`
    // for an ordinary function, which falls straight through to it.
    if (keyStr == "constructor") {
        if (Value ctor = rtFunctionKindConstructor(recv.get()); !ctor.isUndefined()) {
            return ctor.rawBits();
        }
    }
    // After the own properties above, because a static named `call` shadows
    // the inherited one — which is the ordinary rule, and the reason this
    // is not read first even though it is the cheaper lookup.
    if (Value method = rtFunctionMethod(keyStr); !method.isUndefined()) {
        return method.rawBits();
    }
    if (props.get().isObject()) {
        // The chain's end BEFORE the pointer that walks toward it:
        // `rtObjectPrototype` builds %Object.prototype% on first use, so a
        // `propsObj` read out first would be the address the box had before
        // that allocation. Same statement, opposite order, and only one of
        // the two orders survives a collection here.
        const uint64_t objProtoBits = rtObjectPrototype().rawBits();
        ObjectHeader* propsObj = props.get().asObject<ObjectHeader>();
        for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
            ObjectHeader* ancestor = propsObj->protoAncestor(depth);
            if (!ancestor || Value::fromObject(ancestor).rawBits() == objProtoBits) break;
            PropertyInfo inherited;
            if (ancestor->shape &&
                ancestor->shape->lookupProperty(PropertyKey::forString(keyHeader), inherited)) {
                Rooted<Value> key(Value::fromString(keyHeader));
                return propsObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
            }
        }
    }
    rtCheckFunctionMember(keyStr);
    // `Function.prototype` has had its say — `call`, `apply` and `bind`
    // answered above, `constructor` and `toString` refused by name just now
    // — so what is left is the object above it. That step is what makes
    // `f.hasOwnProperty` a function rather than `undefined`, which is the
    // one place bronze answered `undefined` for a member of a prototype it
    // HAS: the nearer, unbuilt one was already diagnosed by name.
    return rtObjectProtoMember(recv, keyStr).rawBits();
}

}  // namespace bronze::runtime
