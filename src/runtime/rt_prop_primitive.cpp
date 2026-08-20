// Property reads on a PRIMITIVE receiver — a string, a number, a boolean, a
// symbol. Its own translation unit for the reason rt_prop.cpp is split by
// receiver kind at all: this is one kind, and it is the one whose answer does
// not come from the receiver.
//
// 7.3.2 GetV boxes a primitive and reads the box. bronze does not build the
// box, because for every member that exists it is unobservable — so what these
// branches decide is where the answer comes from instead, and the answer is now
// the same shape for every one of the four: a REAL intrinsic prototype object,
// walked by the ordinary prototype chain with the PRIMITIVE as the receiver.
//
// That uniformity is recent and it is the point. A string reaching
// `String.prototype` is what gives an index somewhere to fall through TO
// (10.4.3.5), and it is why `"abc"[0]` can answer at all. A number reaching
// `Number.prototype` is what makes `Object.getPrototypeOf(1)` an object a
// program can name, and what makes the `toFixed` it finds there the same
// function object `(1).toFixed` is — where a table consulted beside the value
// has no holder for either question to be about.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The ordinary prototype walk, with the PRIMITIVE as the receiver: an accessor
// found on the chain runs against the value the program wrote, not against the
// intrinsic it was found on. No member of either intrinsic is one today, and
// passing the receiver anyway is what keeps that a fact about the intrinsics
// rather than something this walk assumes.
//
// The intrinsic arrives as a Value and the KEY as a root, which is not
// symmetry for its own sake: materialising either prototype the first time
// allocates a great deal — two wrapper objects, a shape, and every method on
// them — and the key `bronze_elem_get` hands down is an ordinary HEAP string.
// A raw `StringHeader*` across that build is a pointer into dead from-space,
// and the walk then looked up a garbage name and answered `undefined` for
// `"ab"["indexOf"]` on the first computed string member read of a program.
Value protoMember(Value intrinsic, Rooted<Value>& receiver, Rooted<Value>& key,
                  InlineCacheSite* ic) {
    Rooted<Value> proto{intrinsic};
    return proto.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, ic, receiver.slot_ptr());
}

Value stringMember(Value strVal, const std::string& keyStr, Rooted<Value>& key,
                   InlineCacheSite* ic) {
    // Rooted first: the single-code-unit string below allocates, and so can the
    // prototype walk's first touch of the lazily built intrinsic.
    Rooted<Value> self{strVal};

    // 10.4.3.5 StringGetOwnProperty, and it goes FIRST because that is the
    // order 10.4.3 states: a String's index properties are non-writable and
    // non-configurable, so nothing on the chain and nothing a program can
    // define shadows them. A canonical numeric string names one code unit; an
    // index past the end is `undefined` and not an error, because the search
    // simply has nothing more to find.
    uint32_t index = 0;
    if (rtIsIntegerLikeKey(keyStr, index)) {
        return rtStringCharAsString(self.get(), index);
    }
    // 10.4.3.4 StringCreate's own `length`, above the chain for the same reason
    // and below the indices only because a numeric key can never spell it.
    if (keyStr == "length") {
        return Value::fromDouble(self.get().asString<StringHeader>()->getLength());
    }
    const Value found = protoMember(rtStringPrototype(), self, key, ic);
    if (!found.isUndefined()) return found;
    // A full-chain miss, checked against the two holders that were on the chain
    // and in that order: `String.prototype` is the NEARER one, so a name 22.1.3
    // defines and bronze has not built is named as its member rather than as
    // `Object.prototype`'s.
    rtCheckStringMember(keyStr);
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

Value booleanMember(Value boolVal, const std::string& keyStr, Rooted<Value>& key,
                    InlineCacheSite* ic) {
    Rooted<Value> self{boolVal};
    const Value found = protoMember(rtBooleanPrototype(), self, key, ic);
    if (!found.isUndefined()) return found;
    // No nearer table: `Boolean.prototype` is three names (20.3.3) and bronze
    // now answers all three, so the only unimplemented holder left on this
    // chain is `Object.prototype`.
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

Value numberMember(Value numVal, const std::string& keyStr, Rooted<Value>& key, InlineCacheSite* ic) {
    Rooted<Value> self{numVal};
    const Value found = protoMember(rtNumberPrototype(), self, key, ic);
    if (!found.isUndefined()) return found;
    // The two holders on this chain, nearest first: 21.1.3 has one name bronze
    // has not built, and then `Object.prototype` has its own. Answering
    // `undefined` without asking either is what made `(1.5).toFixed(2)` die as
    // "undefined is not a function" instead of naming the member.
    rtCheckNumberProtoMember(keyStr);
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

Value bigintMember(Value bigVal, const std::string& keyStr, Rooted<Value>& key, InlineCacheSite* ic) {
    Rooted<Value> self{bigVal};
    const Value found = protoMember(rtBigIntPrototype(), self, key, ic);
    if (!found.isUndefined()) return found;
    // The two holders on this chain, nearest first, exactly as a number's:
    // `BigInt.prototype` (21.2.3) and then `Object.prototype`. There is no
    // index or `length` step above the walk — a BigInt has no own properties
    // at all, which is what makes this the simplest of the five.
    rtCheckBigIntProtoMember(keyStr);
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

Value symbolMember(Value symVal, const std::string& keyStr, Rooted<Value>& key, InlineCacheSite* ic) {
    Rooted<Value> self{symVal};
    const Value found = protoMember(rtSymbolPrototype(), self, key, ic);
    // No nearer unimplemented holder to name: 20.4.3 defines `constructor`,
    // `description`, `toString`, `valueOf` and two SYMBOL-keyed members, and
    // bronze answers every string-keyed one of them. So an `undefined` here is
    // either 20.4.3.2's own answer for a symbol with no description — which is
    // why this does not treat `undefined` as a miss worth diagnosing — or a
    // genuine miss for `Object.prototype` to have the last word on.
    if (!found.isUndefined()) return found;
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

}  // namespace

Value rtPrimitiveMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                        InlineCacheSite* ic) {
    // The key becomes a ROOT before anything below can allocate. `keyHeader` is
    // valid on entry and is a raw pointer for the rest of this call — the two
    // prototype walks below build their intrinsic on first use, and that build
    // moves a heap key out from under it.
    Rooted<Value> key{Value::fromString(keyHeader)};
    if (objVal.isString()) return stringMember(objVal, keyStr, key, ic);
    if (objVal.isBool()) return booleanMember(objVal, keyStr, key, ic);
    if (objVal.isNumber()) return numberMember(objVal, keyStr, key, ic);
    if (objVal.isSymbol()) return symbolMember(objVal, keyStr, key, ic);
    if (objVal.isBigInt()) return bigintMember(objVal, keyStr, key, ic);
    // Everything a program can name has a branch above; what is left is a tag
    // no program can hold — a hole sentinel that escaped an array. That is not
    // "a property that happens to be absent", so it may not answer `undefined`.
    fatal("unsupported: a property read on this value kind is not implemented "
          "(an array hole is not a value)");
}

}  // namespace bronze::runtime
