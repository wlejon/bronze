// The `+`-chain accumulator: `bronze_concat_begin`, `bronze_concat_append` and
// `bronze_concat_end`, which together are 13.15.3 ApplyStringOrNumericBinary
// Operator applied N-1 times down a left-associative `+` spine.
//
// WHY THREE HELPERS AND NOT ONE. An N-ary concat that took every operand at
// once would be the wrong algorithm, not just a different one. For
// `((a + b) + c) + d` clause 13.15.3 fixes the order as: evaluate a, evaluate
// b, ToPrimitive(a), ToPrimitive(b), decide String-or-numeric and convert,
// THEN evaluate c, ToPrimitive of the accumulator, ToPrimitive(c), and so on.
// An N-ary helper receives its arguments already evaluated, so c's evaluation
// would have preceded a's ToPrimitive — observable whenever `a` is an object
// with a `toString` and evaluating `c` changes what that `toString` reads, and
// a `_lut[i]` in general could be a getter. The accumulator shape has no such
// gap: each `append` is called with exactly the two values the corresponding
// `+` had, at exactly the point that `+` ran.
//
// The one thing it relies on is that ToPrimitive of the ACCUMULATOR is the
// identity. It is: the accumulator is either a String (7.1.1 step 1 returns a
// primitive unchanged, running nothing) or a Number, and neither has a
// `Symbol.toPrimitive`, a `valueOf` or a `toString` that this path consults.
// So splitting `add(add(a, b), c)` into `append(begin(a, b), c)` moves no user
// code across anything.
//
// WHAT `end` IS FOR. While a chain is running, a String accumulator is a
// BUILDER — a real string carrying its text, allocated with room past it
// (string.h). `end` clears that mark, and is the identity on the text and on a
// numeric accumulator alike. Sealing matters because the mark is what licenses
// an in-place append, and the value is about to become something a program can
// hold.

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/profile.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

// How much room the accumulator gets up front, where `seed` is what the first
// `+` produced and `remaining` is how many operands the lowerer knows are
// still to come.
//
// The guess is `seed` plus that many more pieces of about the size of the
// pieces already seen. Both clamps are there because the first piece predicts
// the later ones badly in both directions: `long + long + "."` must not
// reserve a third long one, and `"0" + "0" + ...` eighteen more times must not
// reserve two characters and then reallocate on every step. Being wrong costs
// a reallocation the growth policy would have made anyway — never an answer.
static uint32_t builderCapacityFor(uint32_t seed, uint32_t remaining) {
    uint32_t per = (seed + 1) / 2;
    if (per < 2) per = 2;
    if (per > 32) per = 32;
    uint64_t want = static_cast<uint64_t>(seed) + static_cast<uint64_t>(remaining) * per;
    if (want < 16) want = 16;
    if (want > UINT32_MAX - 1) want = UINT32_MAX - 1;
    return static_cast<uint32_t>(want);
}

// 13.15.3's String branch, with the accumulator in place of the plain concat.
// Both operands are primitives already; `a` may or may not be the builder this
// chain has been filling.
static uint64_t concatIntoBuilder(Rooted<Value>& a, Rooted<Value>& b, uint32_t remaining) {
    // Step 1.d converts the LEFT operand first and then the right. Neither can
    // run user code from here — ToPrimitive is behind us — but a number's
    // digits allocate and a Symbol still refuses, so both stay rooted and the
    // pending cell is tested after each.
    a.set(rtPrimitiveToString(a.get()));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    b.set(rtPrimitiveToString(b.get()));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    Heap& heap = rtHeap();
    const StringHeader* left = a.get().asString<StringHeader>();
    if (!left->isBuilder()) {
        const uint32_t seed = left->length + b.get().asString<StringHeader>()->length;
        StringHeader::startBuilder(heap, a, builderCapacityFor(seed, remaining));
    }
    StringHeader::appendToBuilder(heap, a, b);
    return a.get().rawBits();
}

extern "C" {

uint64_t bronze_concat_begin(uint64_t aBits, uint64_t bBits, uint32_t remaining) {
    recordHelperCall("bronze_concat_begin");
    Value aVal(aBits);
    Value bVal(bBits);
    // The same both-Numbers exit `bronze_dynamic_add` takes, and it is what
    // makes a chain that turns out arithmetic cost nothing: no mark is set, so
    // every later `append` is a plain addition and `end` is a no-op.
    if (aVal.isNumber() && bVal.isNumber()) {
        return Value::fromDouble(aVal.asNumber() + bVal.asNumber()).rawBits();
    }
    Rooted<Value> aRoot{aVal};
    Rooted<Value> bRoot{bVal};
    if (!rtAddToPrimitives(aRoot, bRoot)) return Value::fromUndefined().rawBits();
    if (aRoot.get().isString() || bRoot.get().isString()) {
        return concatIntoBuilder(aRoot, bRoot, remaining);
    }
    return rtAddNonStringTail(aRoot, bRoot);
}

uint64_t bronze_concat_append(uint64_t accBits, uint64_t xBits) {
    recordHelperCall("bronze_concat_append");
    Value accVal(accBits);
    Value xVal(xBits);
    if (accVal.isNumber() && xVal.isNumber()) {
        return Value::fromDouble(accVal.asNumber() + xVal.asNumber()).rawBits();
    }
    Rooted<Value> accRoot{accVal};
    Rooted<Value> xRoot{xVal};
    // ToPrimitive of the accumulator runs first and does nothing, then
    // ToPrimitive of the operand runs whatever the program put on it — which
    // is the order this `+` would have had on its own.
    if (!rtAddToPrimitives(accRoot, xRoot)) return Value::fromUndefined().rawBits();
    if (accRoot.get().isString() || xRoot.get().isString()) {
        // No hint left to give: a chain that only became a String here (`1 + 2
        // + 'x' + …`) has no count of what follows, and pays for it in the
        // growth policy rather than in an answer.
        return concatIntoBuilder(accRoot, xRoot, 0);
    }
    return rtAddNonStringTail(accRoot, xRoot);
}

uint64_t bronze_concat_end(uint64_t vBits) {
    recordHelperCall("bronze_concat_end");
    Value v(vBits);
    // A numeric chain, or a String one the seam left as an ordinary concat:
    // both arrive here unmarked and clearing an unset bit is what "identity"
    // means. Nothing is copied and nothing is shrunk — the slack a builder
    // still carries is bounded by the growth policy, and a copy to shed it
    // would cost the allocation this whole shape exists to remove.
    if (v.isString()) v.asString<StringHeader>()->sealBuilder();
    return vBits;
}

}  // extern "C"

}  // namespace bronze::runtime
