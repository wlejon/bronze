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
#include "runtime/number_format.h"
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
        const uint64_t seed64 = static_cast<uint64_t>(left->length) + b.get().asString<StringHeader>()->length;
        // A long left operand is not copied into an accumulator of this chain:
        // it is appended to as a view (StringHeader::concat), in place when it
        // is the string a loop keeps extending, and so is every later piece.
        if (seed64 >= StringHeader::kViewMinLength) {
            return StringHeader::concat(heap, a, b).rawBits();
        }
        const uint32_t seed = static_cast<uint32_t>(seed64);
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

// A template's pieces are mostly a few characters, where a call to memcpy
// costs more than the copy: two overlapping words cover 4 to 16 bytes, and
// three single bytes cover 1 to 3, with no access outside either string.
static inline void copyLatin1(char* dst, const char* src, uint32_t n) {
    if (n > 16) {
        std::memcpy(dst, src, n);
    } else if (n >= 8) {
        uint64_t a, b;
        std::memcpy(&a, src, 8);
        std::memcpy(&b, src + n - 8, 8);
        std::memcpy(dst, &a, 8);
        std::memcpy(dst + n - 8, &b, 8);
    } else if (n >= 4) {
        uint32_t a, b;
        std::memcpy(&a, src, 4);
        std::memcpy(&b, src + n - 4, 4);
        std::memcpy(dst, &a, 4);
        std::memcpy(dst + n - 4, &b, 4);
    } else if (n > 0) {
        dst[0] = src[0];
        dst[n / 2] = src[n / 2];
        dst[n - 1] = src[n - 1];
    }
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
        if (a->isLatin1() && b->isLatin1() && !a->isBuilder() && seed < StringHeader::kViewMinLength) {
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

}  // extern "C"

namespace {

// A template piece that is a Number, as its String (written back into the
// block by the caller, which keeps it rooted). Runs nothing: a small
// non-negative integer (an id, a count, a counter) is the cached string, -0
// included; any other Number is formatted, which allocates.
Value numberPiece(Value v) {
    const double d = v.asNumber();
    if (d >= 0.0 && d < 1024.0) {
        const auto n = static_cast<uint32_t>(d);
        if (static_cast<double>(n) == d) {
            if (const Value cached = rtSmallIntString(n); cached.isString()) return cached;
        }
    }
    return rtPrimitiveToString(v);
}

// Any template: every piece converted, then the result sized, allocated and
// filled. Pieces already converted by the caller are Strings and pass.
uint64_t concatTemplateGeneral(uint64_t* pieces, uint32_t count) {
    uint64_t total = 0;
    bool utf16 = false;
    for (uint32_t i = 0; i < count; ++i) {
        Value v(pieces[i]);
        if (!v.isString()) {
            v = numberPiece(v);
            pieces[i] = v.rawBits();
        }
        const StringHeader* s = v.asString<StringHeader>();
        total += s->length;
        utf16 = utf16 || s->isUTF16();
    }
    Heap& heap = rtHeap();
    if (total >= StringHeader::kViewMinLength) {
        // A long result: pairwise, as `+` builds one, so it is a view a loop
        // can go on extending in place.
        Rooted<Value> acc{Value(pieces[0])};
        Rooted<Value> next{Value()};
        for (uint32_t i = 1; i < count; ++i) {
            next.set(Value(pieces[i]));
            acc.set(StringHeader::concat(heap, acc, next));
        }
        return acc.get().rawBits();
    }
    const auto len = static_cast<uint32_t>(total);
    const size_t unit = utf16 ? sizeof(uint16_t) : 1;
    HeapObjectHeader* h =
        heap.tryAllocate(2 * sizeof(uint32_t) + (static_cast<size_t>(len) + 1) * unit, Tag::String);
    StringHeader* out = nullptr;
    if (h != nullptr) {
        out = reinterpret_cast<StringHeader*>(h);
        out->header.flags = 0;
        out->length = len;
        out->flags = utf16 ? StringHeader::kUTF16Flag : 0;
    } else {
        // Collects: the pieces are re-read from the block below.
        out = StringHeader::createBuilder(heap, utf16, len, len);
        out->sealBuilder();
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const StringHeader* s = Value(pieces[i]).asString<StringHeader>();
        const uint32_t n = s->length;
        if (!utf16) {
            copyLatin1(out->latin1Data() + at, s->latin1Data(), n);
        } else if (s->isUTF16()) {
            std::memcpy(out->utf16Data() + at, s->utf16Data(), static_cast<size_t>(n) * sizeof(uint16_t));
        } else {
            const auto* in = reinterpret_cast<const unsigned char*>(s->latin1Data());
            uint16_t* dst = out->utf16Data() + at;
            for (uint32_t k = 0; k < n; ++k) dst[k] = in[k];
        }
        at += n;
    }
    if (utf16) {
        out->utf16Data()[len] = 0;
    } else {
        out->latin1Data()[len] = '\0';
    }
    return Value::fromString(out).rawBits();
}

}  // namespace

extern "C" {

uint64_t bronze_concat_template(uint64_t* pieces, uint32_t count) {
    recordHelperCall("bronze_concat_template");
    // A template's pieces in a block the caller owns and the collector scans
    // (the argv block): each a String, or a Number its `to.string` let
    // through.
    //
    // The usual template (a key, a label: one-byte pieces, short) in ONE pass:
    // each String's units and each Number's ToString (formatJsNumber,
    // nothing allocated) are copied into a buffer on the stack, then the
    // result is allocated and filled with one copy. A two-byte piece, a
    // result long enough to be a view, or a full young window is the general
    // path's, which converts a Number into its String and writes it back into
    // the block, so it stays rooted while later pieces are converted.
    char text[StringHeader::kViewMinLength];
    uint32_t len = 0;
    uint32_t i = 0;
    for (; i < count; ++i) {
        const Value v(pieces[i]);
        if (!v.isString()) {
            // A Number's ToString written straight into the buffer: nothing
            // allocated (formatJsNumber writes at most 32 bytes).
            if (len > StringHeader::kViewMinLength - 32) break;
            const auto n = static_cast<uint32_t>(formatJsNumber(v.asNumber(), text + len));
            if (n >= StringHeader::kViewMinLength - len) break;
            len += n;
            continue;
        }
        const StringHeader* s = v.asString<StringHeader>();
        const uint32_t n = s->length;
        if (s->isUTF16() || n >= StringHeader::kViewMinLength - len) break;
        copyLatin1(text + len, s->latin1Data(), n);
        len += n;
    }
    if (i == count) {
        if (HeapObjectHeader* h = rtHeap().tryAllocate(2 * sizeof(uint32_t) + static_cast<size_t>(len) + 1,
                                                       Tag::String)) {
            auto* out = reinterpret_cast<StringHeader*>(h);
            out->length = len;
            out->flags = 0;
            char* dst = reinterpret_cast<char*>(out + 1);
            copyLatin1(dst, text, len);
            dst[len] = '\0';
            return Value::fromString(out).rawBits();
        }
    }
    return concatTemplateGeneral(pieces, count);
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
