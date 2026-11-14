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
#include <cstring>

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

static_assert(offsetof(StringHeader, header) + offsetof(HeapObjectHeader, flags) ==
                  BRONZE_ABI_OBJ_FLAGS_OFFSET,
              "HeapObjectHeader::flags offset mismatch");
static_assert(offsetof(StringHeader, header) + offsetof(HeapObjectHeader, size) ==
                  BRONZE_ABI_HDR_SIZE_OFFSET,
              "HeapObjectHeader::size offset mismatch");
static_assert(StringHeader::kBuilderFlag == BRONZE_ABI_STRING_BUILDER_BIT,
              "StringHeader::kBuilderFlag mismatch");
static_assert(offsetof(StringHeader, length) == BRONZE_ABI_STRING_LENGTH_OFFSET,
              "StringHeader::length offset mismatch");
static_assert(offsetof(StringHeader, flags) == BRONZE_ABI_STRING_FLAGS_OFFSET,
              "StringHeader::flags offset mismatch");
static_assert(sizeof(StringHeader) == BRONZE_ABI_STRING_DATA_OFFSET,
              "StringHeader data payload offset mismatch");

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
    // digits allocate and a Symbol still refuses, so both stay rooted.
    a.set(rtPrimitiveToString(a.get()));
    b.set(rtPrimitiveToString(b.get()));

    Heap& heap = rtHeap();
    const StringHeader* left = a.get().asString<StringHeader>();
    if (!left->isBuilder()) {
        const uint32_t seed = left->length + b.get().asString<StringHeader>()->length;
        StringHeader::startBuilder(heap, a, builderCapacityFor(seed, remaining));
    }
    StringHeader::appendToBuilder(heap, a, b);
    return a.get().rawBits();
}

// The two helpers' general paths, out of line: they root their operands, and
// a frame holding roots is one the fast paths above them should not have to
// set up to copy a few characters.
BRONZE_NOINLINE static uint64_t concatBeginSlow(uint64_t aBits, uint64_t bBits, uint32_t remaining) {
    Rooted<Value> aRoot{Value(aBits)};
    Rooted<Value> bRoot{Value(bBits)};
    if (aRoot.get().isString() && bRoot.get().isString()) return concatIntoBuilder(aRoot, bRoot, remaining);
    if (!rtAddToPrimitives(aRoot, bRoot)) return Value::fromUndefined().rawBits();
    if (aRoot.get().isString() || bRoot.get().isString()) {
        return concatIntoBuilder(aRoot, bRoot, remaining);
    }
    return rtAddNonStringTail(aRoot, bRoot);
}

BRONZE_NOINLINE static uint64_t concatAppendSlow(uint64_t accBits, uint64_t xBits) {
    Rooted<Value> accRoot{Value(accBits)};
    Rooted<Value> xRoot{Value(xBits)};
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
    if (aVal.isString() && bVal.isString()) {
        // Two Latin-1 strings, the chain's usual start: the builder is taken
        // from the young window when it has the room, which collects nothing,
        // so neither operand needs a root and the two texts are copied
        // straight in. Anything else (a builder on the left, UTF-16, a full
        // window, a length past 32 bits) is the general path's.
        const StringHeader* a = aVal.asString<StringHeader>();
        const StringHeader* b = bVal.asString<StringHeader>();
        const uint64_t seed = static_cast<uint64_t>(a->length) + b->length;
        if (a->isLatin1() && b->isLatin1() && !a->isBuilder() && seed < (uint64_t{1} << 31)) {
            const uint32_t cap = builderCapacityFor(static_cast<uint32_t>(seed), remaining);
            if (HeapObjectHeader* h = rtHeap().tryAllocate(2 * sizeof(uint32_t) + static_cast<size_t>(cap) + 1,
                                                           Tag::String)) {
                auto* s = reinterpret_cast<StringHeader*>(h);
                s->header.flags = StringHeader::kBuilderFlag;
                s->length = static_cast<uint32_t>(seed);
                s->flags = 0;
                char* text = s->latin1Data();
                std::memcpy(text, a->latin1Data(), a->length);
                std::memcpy(text + a->length, b->latin1Data(), b->length);
                text[seed] = '\0';
                return Value::fromString(s).rawBits();
            }
        }
    }
    return concatBeginSlow(aBits, bBits, remaining);
}

uint64_t bronze_concat_append(uint64_t accBits, uint64_t xBits) {
    recordHelperCall("bronze_concat_append");
    Value accVal(accBits);
    Value xVal(xBits);
    if (accVal.isNumber() && xVal.isNumber()) {
        return Value::fromDouble(accVal.asNumber() + xVal.asNumber()).rawBits();
    }
    if (accVal.isString() && xVal.isString()) {
        StringHeader* b = accVal.asString<StringHeader>();
        const StringHeader* s = xVal.asString<StringHeader>();
        if (b->isBuilder()) {
            const uint32_t addLen = s->length;
            if (addLen == 0) return accBits;
            const uint32_t oldLen = b->length;
            const uint32_t newLen = oldLen + addLen;
            const bool needUTF16 = b->isUTF16() || s->isUTF16();
            if (needUTF16 == b->isUTF16() && b->capacity() >= newLen) {
                if (needUTF16) {
                    if (s->isUTF16()) {
                        std::memcpy(b->utf16Data() + oldLen, s->utf16Data(), addLen * sizeof(uint16_t));
                    } else {
                        const unsigned char* in = reinterpret_cast<const unsigned char*>(s->latin1Data());
                        uint16_t* out = b->utf16Data() + oldLen;
                        for (uint32_t i = 0; i < addLen; ++i) out[i] = in[i];
                    }
                    b->utf16Data()[newLen] = 0;
                } else {
                    std::memcpy(b->latin1Data() + oldLen, s->latin1Data(), addLen);
                    b->latin1Data()[newLen] = '\0';
                }
                b->length = newLen;
                b->flags &= StringHeader::kUTF16Flag;
                return accBits;
            }
        }
    }
    return concatAppendSlow(accBits, xBits);
}

uint64_t bronze_concat_strings(const uint64_t* pieces, uint32_t count) {
    recordHelperCall("bronze_concat_strings");
    // Every piece a sealed Latin-1 String and room in the young window: the
    // result is allocated at its length with nothing collected on the way, so
    // the texts are copied straight out of the caller's block.
    uint64_t total = 0;
    bool plain = true;
    for (uint32_t i = 0; i < count; ++i) {
        const Value v(pieces[i]);
        if (!v.isString()) {
            plain = false;
            break;
        }
        const StringHeader* s = v.asString<StringHeader>();
        plain = plain && s->isLatin1() && !s->isBuilder();
        total += s->length;
    }
    if (plain && total < (uint64_t{1} << 31)) {
        if (HeapObjectHeader* h =
                rtHeap().tryAllocate(2 * sizeof(uint32_t) + static_cast<size_t>(total) + 1, Tag::String)) {
            auto* out = reinterpret_cast<StringHeader*>(h);
            out->header.flags = 0;
            out->length = static_cast<uint32_t>(total);
            out->flags = 0;
            char* text = out->latin1Data();
            for (uint32_t i = 0; i < count; ++i) {
                const StringHeader* s = Value(pieces[i]).asString<StringHeader>();
                std::memcpy(text, s->latin1Data(), s->length);
                text += s->length;
            }
            *text = '\0';
            return Value::fromString(out).rawBits();
        }
    }
    // Anything else is the accumulator's, a step at a time. The block is the
    // caller's root, so each piece is read after whatever the step before it
    // collected.
    Rooted<Value> acc{Value(bronze_concat_begin(pieces[0], pieces[1], count - 2))};
    for (uint32_t i = 2; i < count; ++i) {
        acc.set(Value(bronze_concat_append(acc.get().rawBits(), pieces[i])));
    }
    return bronze_concat_end(acc.get().rawBits());
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
