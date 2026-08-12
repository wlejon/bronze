// Property reads on a PRIMITIVE receiver — a string, a number, a boolean, a
// symbol. Its own translation unit for the reason rt_prop.cpp is split by
// receiver kind at all: this is one kind, and it is the one whose answer does
// not come from the receiver.
//
// 7.3.2 GetV boxes a primitive and reads the box. bronze does not build the
// box, because for every member that exists it is unobservable — so what these
// branches decide is where the answer comes from instead. Two arrangements live
// here, and the difference between them is the difference between a member that
// can be reached and one that can only be handed out:
//
//   A string reaches `String.prototype` and a boolean reaches
//   `Boolean.prototype`, which are REAL objects, walked by the ordinary
//   prototype chain. That is what gives an index somewhere to fall through TO
//   (10.4.3.5), and it is why `"abc"[0]` can answer at all.
//
//   A number and a symbol still get their members handed out BESIDE the value
//   from a table. `Number` is a namespace object in bronze and not a
//   constructor, so there is no `Number.prototype` for one to point at yet;
//   that is its own gap and named as one.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
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
Value protoMember(Value intrinsic, Rooted<Value>& receiver, StringHeader* keyHeader,
                  InlineCache* ic) {
    Rooted<Value> proto{intrinsic};
    Rooted<Value> key{Value::fromString(keyHeader)};
    return proto.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, ic, receiver.slot_ptr());
}

Value stringMember(Value strVal, const std::string& keyStr, StringHeader* keyHeader,
                   InlineCache* ic) {
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
    const Value found = protoMember(rtStringPrototype(), self, keyHeader, ic);
    if (!found.isUndefined()) return found;
    // A full-chain miss, checked against the two holders that were on the chain
    // and in that order: `String.prototype` is the NEARER one, so a name 22.1.3
    // defines and bronze has not built is named as its member rather than as
    // `Object.prototype`'s.
    rtCheckStringMember(keyStr);
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

Value booleanMember(Value boolVal, const std::string& keyStr, StringHeader* keyHeader,
                    InlineCache* ic) {
    Rooted<Value> self{boolVal};
    const Value found = protoMember(rtBooleanPrototype(), self, keyHeader, ic);
    if (!found.isUndefined()) return found;
    // No nearer table: `Boolean.prototype` is three names (20.3.3) and bronze
    // now answers all three, so the only unimplemented holder left on this
    // chain is `Object.prototype`.
    rtObjectProtoCheckMissingMember(keyStr);
    return Value::fromUndefined();
}

}  // namespace

Value rtPrimitiveMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                        InlineCache* ic) {
    if (objVal.isString()) return stringMember(objVal, keyStr, keyHeader, ic);
    if (objVal.isBool()) return booleanMember(objVal, keyStr, keyHeader, ic);
    // A primitive NUMBER. Answering `undefined` here is what made
    // `(1.5).toFixed(2)` die as "undefined is not a function" instead of naming
    // the member, which is the silent fallback the loud-member rule exists to
    // prevent.
    if (objVal.isNumber()) {
        const Value method = rtNumberMethod(keyStr);
        if (!method.isUndefined()) return method;
        rtCheckNumberProtoMember(keyStr);
        return Value::fromUndefined();
    }
    // A primitive SYMBOL — `sym.toString()`, `sym.description`.
    if (objVal.isSymbol()) return rtSymbolMember(objVal, keyStr);
    // Everything a program can name has a branch above; what is left is a tag
    // no program can hold — a hole sentinel that escaped an array. That is not
    // "a property that happens to be absent", so it may not answer `undefined`.
    fatal("unsupported: a property read on this value kind is not implemented "
          "(an array hole is not a value)");
}

}  // namespace bronze::runtime
