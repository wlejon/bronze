// `String.prototype[Symbol.iterator]` (ECMA-262 22.1.3.36) and the
// StringIterator object it hands back (22.1.5).
//
// The method is installed as an own SYMBOL-KEYED property of the real
// `String.prototype` object, so a primitive string reaches it by the ordinary
// prototype walk — no special case on the property path, which is what
// removed the named refusal rt_prop.cpp used to carry for exactly this key.
//
// `for-of` over a string never comes here: iterator.cpp steps a string by
// code point with a cursor and no object. This file is for the program that
// HOLDS the iterator, and the two walks must agree about every step — both
// step by CODE POINT, so a surrogate pair is one iteration yielding a
// two-unit string, and both come down to the same isSurrogatePair test.

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

constexpr uint16_t kHighSurrogateFirst = 0xD800;
constexpr uint16_t kHighSurrogateLast = 0xDBFF;
constexpr uint16_t kLowSurrogateFirst = 0xDC00;
constexpr uint16_t kLowSurrogateLast = 0xDFFF;

bool isSurrogatePair(uint16_t high, uint16_t low) {
    return high >= kHighSurrogateFirst && high <= kHighSurrogateLast &&
           low >= kLowSurrogateFirst && low <= kLowSurrogateLast;
}

Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

uint64_t stringIterNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::String)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    Rooted<Value> target{readSlot(self, StringIteratorSlot::IteratedString)};
    Rooted<Value> none;
    if (!target.get().isString()) return iterResult(none, true).rawBits();
    const auto at =
        static_cast<uint32_t>(readSlot(self, StringIteratorSlot::NextIndex).asNumber());
    const StringHeader* str = target.get().asString<StringHeader>();
    const uint32_t len = str->getLength();
    if (at >= len) return iterResult(none, true).rawBits();

    // One CODE POINT per step (22.1.5.1's CodePointAt), which is the whole
    // difference between this and an index walk: a surrogate pair is a single
    // iteration whose value is a two-unit string.
    const uint16_t unit = str->charCodeAt(at);
    const bool pair = at + 1 < len && isSurrogatePair(unit, str->charCodeAt(at + 1));
    Value piece;
    if (pair) {
        const uint16_t units[2] = {unit, str->charCodeAt(at + 1)};
        piece = Value::fromString(StringHeader::createUTF16(rtHeap(), units, 2));
    } else if (unit < 0x100) {
        const char byte = static_cast<char>(unit);
        piece = Value::fromString(StringHeader::createLatin1(rtHeap(), &byte, 1));
    } else {
        piece = Value::fromString(StringHeader::createUTF16(rtHeap(), &unit, 1));
    }
    // The piece's allocation may have moved the iterator; the write below goes
    // through the root, so only the produced value had to be re-held.
    Rooted<Value> produced{piece};
    writeSlot(self, StringIteratorSlot::NextIndex,
              Value::fromDouble(static_cast<double>(at + (pair ? 2 : 1))));
    return iterResult(produced, false).rawBits();
}

// 22.1.3.36's body. The receiver comes through thisStringValue's little
// sibling — RequireObjectCoercible plus ToString — so a String wrapper's
// iterator walks its [[StringData]] and a detached call on `null` throws.
uint64_t stringIterator(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    Value data;
    if (!rtThisStringValue(self.get(), data)) {
        return rtThrowTypeError(
                   "String.prototype[Symbol.iterator] called on a value that is not a string")
            .rawBits();
    }
    Rooted<Value> str{data};
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::String)};
    Rooted<Value> nextFn{rtNativeFunction(stringIterNext, 0)};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    writeSlot(it, StringIteratorSlot::IteratedString, str.get());
    writeSlot(it, StringIteratorSlot::NextIndex, Value::fromDouble(0.0));
    return it.get().rawBits();
}

}  // namespace

// Installed by builtin_wrappers.cpp's one intrinsic initializer, beside the
// string methods — a DEFINITION with `enumerable: false`, which is what
// 22.1.3.36 says the property is.
void rtInstallStringIterator(Rooted<Value>& proto) {
    Rooted<Value> key{rtIteratorKey()};
    Rooted<Value> fn{rtNativeFunction(stringIterator, 0)};
    proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn, /*ic=*/nullptr,
                                                  /*enumerable=*/false, /*defineOwn=*/true);
}

}  // namespace bronze::runtime
