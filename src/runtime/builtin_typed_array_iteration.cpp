#include <bit>

#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"

namespace bronze::runtime {

namespace {

enum IterKind : uint32_t { Keys = 0, Values = 1, Entries = 2 };

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

Value makePair(Rooted<Value>& a, Rooted<Value>& b) {
    Rooted<Value> pair{Value(bronze_create_array(2))};
    pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, a);
    pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, b);
    return pair.get();
}

uint64_t taIterNext(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::Array)) {
        return rtThrowTypeError("next called on an incompatible receiver").rawBits();
    }
    Rooted<Value> target{readSlot(self, ArrayIteratorSlot::IteratedArrayLike)};
    Rooted<Value> none;
    if (!isTypedArray(target.get())) return iterResult(none, true).rawBits();
    auto* view = target.get().asObject<TypedArrayHeader>();
    if (view->buffer.isObject() &&
        view->buffer.asObject<ArrayBufferHeader>()->isDetached()) {
        return rtThrowTypeError("ArrayBuffer is detached").rawBits();
    }
    // 23.1.5.1 asks IsTypedArrayOutOfBounds before the length: iterating a
    // view a shrinking `resize` stranded is a TypeError, not an early `done`.
    if (view->isOutOfBounds()) {
        return rtThrowTypeError("TypedArray is out of bounds of its ArrayBuffer").rawBits();
    }

    const auto at = static_cast<uint32_t>(readSlot(self, ArrayIteratorSlot::NextIndex).asNumber());
    if (at >= lengthOf(target.get())) return iterResult(none, true).rawBits();
    const auto kind = static_cast<uint32_t>(readSlot(self, ArrayIteratorSlot::Kind).asNumber());

    writeSlot(self, ArrayIteratorSlot::NextIndex, Value::fromDouble(static_cast<double>(at + 1)));

    Rooted<Value> produced;
    if (kind == Keys) {
        produced.set(Value::fromDouble(static_cast<double>(at)));
    } else if (kind == Values) {
        produced.set(rtTypedArrayElement(target.get(), at));
    } else {
        Rooted<Value> index{Value::fromDouble(static_cast<double>(at))};
        Rooted<Value> val{rtTypedArrayElement(target.get(), at)};
        produced.set(makePair(index, val));
    }
    return iterResult(produced, false).rawBits();
}

uint64_t makeTypedArrayIterator(uint64_t thisBits, uint32_t kind, const char* method) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), method)) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> it{rtNewIteratorObject(IteratorProto::Array)};
    Rooted<Value> nextFn{rtNativeFunction(taIterNext, 0)};
    Rooted<Value> nk{rtMakeString("next")};
    it.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nk, nextFn);
    writeSlot(it, ArrayIteratorSlot::IteratedArrayLike, self.get());
    writeSlot(it, ArrayIteratorSlot::NextIndex, Value::fromDouble(0.0));
    writeSlot(it, ArrayIteratorSlot::Kind, Value::fromDouble(static_cast<double>(kind)));
    return it.get().rawBits();
}

}  // namespace

uint64_t taIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "indexOf")) return Value::fromUndefined().rawBits();

    if (isBigIntView(self.get())) {
        const uint32_t len = lengthOf(self.get());
        // 23.2.3.17 never converts the needle: IsStrictlyEqual across types is
        // simply false, so a non-BigInt needle finds nothing and throws
        // nothing. Each element materialises as a BigInt for the compare —
        // strict equality is mathematical, so a needle no 64-bit element could
        // ever equal answers -1 by comparison, not by a wrapping shortcut.
        uint32_t from = 0;
        relativeArg(args[1], len, from, 0);
        for (uint32_t i = from; i < len; ++i) {
            Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
            if (bronze_strict_eq(elem.get().rawBits(), args[0].rawBits())) {
                return Value::fromDouble(i).rawBits();
            }
        }
        return Value::fromDouble(-1.0).rawBits();
    }

    // The same rule for a Number view: no ToNumber on the needle, so a
    // string, BigInt or undefined needle answers -1 rather than matching or
    // throwing — after the fromIndex conversion has run its side effects
    // (step 4 precedes the loop).
    const uint32_t len = lengthOf(self.get());
    uint32_t from = 0;
    relativeArg(args[1], len, from, 0);
    if (!args[0].isNumber()) return Value::fromDouble(-1.0).rawBits();
    const double needle = args[0].asNumber();

    for (uint32_t i = from; i < len; ++i) {
        if (elemOf(self.get(), i) == needle) return Value::fromDouble(i).rawBits();
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t taLastIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "lastIndexOf")) return Value::fromUndefined().rawBits();

    const bool big = isBigIntView(self.get());

    const uint32_t len = lengthOf(self.get());
    if (len == 0) return Value::fromDouble(-1.0).rawBits();

    double fromNum = args.count() > 1 && !args[1].isUndefined()
                         ? toInteger(rtToNumber(args[1]))
                         : static_cast<double>(len - 1);
    double k = fromNum >= 0 ? std::min(fromNum, static_cast<double>(len - 1))
                            : static_cast<double>(len) + fromNum;
    if (k < 0) return Value::fromDouble(-1.0).rawBits();

    // 23.2.3.20 never converts the needle either — a needle of the wrong type
    // answers -1 after the fromIndex conversion above has run.
    if (!big && !args[0].isNumber()) return Value::fromDouble(-1.0).rawBits();
    const double needle = big ? 0.0 : args[0].asNumber();

    for (int64_t i = static_cast<int64_t>(k); i >= 0; --i) {
        if (big) {
            // The un-converted needle and the strict compare, exactly as
            // indexOf above says.
            Rooted<Value> elem{rtTypedArrayElement(self.get(), static_cast<uint32_t>(i))};
            if (bronze_strict_eq(elem.get().rawBits(), args[0].rawBits())) {
                return Value::fromDouble(static_cast<double>(i)).rawBits();
            }
        } else if (elemOf(self.get(), static_cast<uint32_t>(i)) == needle) {
            return Value::fromDouble(static_cast<double>(i)).rawBits();
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t taIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "includes")) return Value::fromUndefined().rawBits();

    if (isBigIntView(self.get())) {
        // SameValueZero on BigInts IS strict equality — no NaN and one zero —
        // and the needle is never converted, as in indexOf above.
        const uint32_t len = lengthOf(self.get());
        uint32_t from = 0;
        relativeArg(args[1], len, from, 0);
        for (uint32_t i = from; i < len; ++i) {
            Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
            if (bronze_strict_eq(elem.get().rawBits(), args[0].rawBits())) {
                return Value::fromBool(true).rawBits();
            }
        }
        return Value::fromBool(false).rawBits();
    }

    // SameValueZero across types is false, so 23.2.3.16's needle is never
    // converted either — `includes(null)` on a zero-filled view is false, not
    // the true a ToNumber(null) == 0 shortcut would answer.
    const uint32_t len = lengthOf(self.get());
    uint32_t from = 0;
    relativeArg(args[1], len, from, 0);
    if (!args[0].isNumber()) return Value::fromBool(false).rawBits();
    const double needle = args[0].asNumber();

    for (uint32_t i = from; i < len; ++i) {
        const double v = elemOf(self.get(), i);
        if (std::isnan(needle) && std::isnan(v)) return Value::fromBool(true).rawBits();
        if (v == needle) return Value::fromBool(true).rawBits();
    }
    return Value::fromBool(false).rawBits();
}

template <bool Reverse, bool WantIndex>
static uint64_t taFindImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "find")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "find")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    const uint32_t len = lengthOf(self.get());
    for (uint32_t n = 0; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        Rooted<Value> found{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(found.get().rawBits())) {
            return WantIndex ? Value::fromDouble(i).rawBits() : elem.get().rawBits();
        }
    }
    return WantIndex ? Value::fromDouble(-1.0).rawBits() : Value::fromUndefined().rawBits();
}

uint64_t taFind(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return taFindImpl<false, false>(env, thisBits, argc, argv);
}

uint64_t taFindIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return taFindImpl<false, true>(env, thisBits, argc, argv);
}

uint64_t taFindLast(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return taFindImpl<true, false>(env, thisBits, argc, argv);
}

uint64_t taFindLastIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return taFindImpl<true, true>(env, thisBits, argc, argv);
}

uint64_t taForEach(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        callBack(fn, thisArg, elem, i, self);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return Value::fromUndefined().rawBits();
}

uint64_t taMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "map")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "map")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    const uint32_t len = lengthOf(self.get());
    const bool big = isBigIntView(self.get());
    Rooted<Value> out{newViewLike(self.get(), len)};
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        Rooted<Value> mapped{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        // The result converts with the RESULT view's content type — ToBigInt
        // here (a Number mapped into a BigInt view is 7.1.13's TypeError),
        // ToNumber for the other ten kinds.
        if (big) {
            uint64_t bits = 0;
            if (!rtBigIntToRawBits64(mapped.get(), bits)) {
                return Value::fromUndefined().rawBits();
            }
            out.get().asObject<TypedArrayHeader>()->setRawBits64(i, bits);
        } else {
            const double v = rtToNumber(mapped.get());
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            out.get().asObject<TypedArrayHeader>()->set(i, v);
        }
    }
    return out.get().rawBits();
}

uint64_t taFilter(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "filter")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "filter")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    const uint32_t len = lengthOf(self.get());
    const bool big = isBigIntView(self.get());
    // Kept elements stage as raw payloads — the stored eight bytes for a
    // BigInt kind, a double's bits otherwise — because the callback can
    // collect, and a staged vector of BigInt VALUES would be invisible to the
    // collector.
    std::vector<uint64_t> kept;
    for (uint32_t i = 0; i < len; ++i) {
        const uint64_t raw =
            big ? bitsOf(self.get(), i) : std::bit_cast<uint64_t>(elemOf(self.get(), i));
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        Rooted<Value> res{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(res.get().rawBits())) {
            kept.push_back(raw);
        }
    }
    Rooted<Value> out{newViewLike(self.get(), static_cast<uint32_t>(kept.size()))};
    auto* dst = out.get().asObject<TypedArrayHeader>();
    for (uint32_t i = 0; i < kept.size(); ++i) {
        if (big) {
            dst->setRawBits64(i, kept[i]);
        } else {
            dst->set(i, std::bit_cast<double>(kept[i]));
        }
    }
    return out.get().rawBits();
}

uint64_t taEvery(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "every")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "every")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        Rooted<Value> res{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (!bronze_truthy(res.get().rawBits())) {
            return Value::fromBool(false).rawBits();
        }
    }
    return Value::fromBool(true).rawBits();
}

uint64_t taSome(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "some")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "some")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    const uint32_t len = lengthOf(self.get());
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        Rooted<Value> res{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(res.get().rawBits())) {
            return Value::fromBool(true).rawBits();
        }
    }
    return Value::fromBool(false).rawBits();
}

template <bool Reverse>
static uint64_t taReduceImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "reduce")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "reduce")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    Rooted<Value> acc{Value::fromUndefined()};
    uint32_t next = 0;
    if (args.count() > 1) {
        acc.set(args[1]);
    } else {
        if (len == 0) {
            return rtThrowTypeError("Reduce of empty array with no initial value").rawBits();
        }
        acc.set(rtTypedArrayElement(self.get(), Reverse ? len - 1 : 0));
        next = 1;
    }

    for (uint32_t n = next; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        Rooted<Value> elem{rtTypedArrayElement(self.get(), i)};
        Value block[4] = {acc.get(), elem.get(), Value::fromDouble(static_cast<double>(i)),
                          self.get()};
        acc.set(Value(bronze_dynamic_call(fn.get().rawBits(), BRONZE_ABI_UNDEFINED_BITS, 4,
                                          reinterpret_cast<const uint64_t*>(block))));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return acc.get().rawBits();
}

uint64_t taReduce(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return taReduceImpl<false>(env, thisBits, argc, argv);
}

uint64_t taReduceRight(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return taReduceImpl<true>(env, thisBits, argc, argv);
}

uint64_t taKeys(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return makeTypedArrayIterator(thisBits, Keys, "keys");
}

uint64_t taValues(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return makeTypedArrayIterator(thisBits, Values, "values");
}

uint64_t taEntries(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return makeTypedArrayIterator(thisBits, Entries, "entries");
}

Value rtTypedArrayIteratorMethod() { return rtNativeFunction(taValues, 0); }

}  // namespace bronze::runtime
