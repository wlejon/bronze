#include "runtime/builtin_array_internal.h"
#include "runtime/rt_roots.h"
#include "runtime/tls_block.h"
#include "abi/bronze_abi.h"

namespace bronze::runtime {

uint64_t arrayForEach(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "forEach")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "forEach")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    for (uint32_t i = 0; i < len; ++i) {
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        callBack(fn, thisArg, elem, i, self);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return Value::fromUndefined().rawBits();
}

uint64_t arrayMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "map")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "map")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    Rooted<Value> out{rtArraySpeciesCreate(self, len)};
    for (uint32_t i = 0; i < len; ++i) {
        if (!rtArrayLikeHasElement(self, i)) {
            continue;
        }
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Rooted<Value> mapped{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        rtCreateDataPropertyOrThrow(out, i, mapped);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return out.get().rawBits();
}

uint64_t arrayFilter(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "filter")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "filter")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    uint32_t to = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Rooted<Value> kept{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(kept.get().rawBits())) {
            rtCreateDataPropertyOrThrow(out, to++, elem);
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        }
    }
    return out.get().rawBits();
}

uint64_t arraySome(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "some")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "some")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    for (uint32_t i = 0; i < len; ++i) {
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Rooted<Value> hit{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(hit.get().rawBits())) {
            return Value::fromBool(true).rawBits();
        }
    }
    return Value::fromBool(false).rawBits();
}

uint64_t arrayEvery(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "every")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "every")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    for (uint32_t i = 0; i < len; ++i) {
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Rooted<Value> held{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (!bronze_truthy(held.get().rawBits())) {
            return Value::fromBool(false).rawBits();
        }
    }
    return Value::fromBool(true).rawBits();
}

template <bool Reverse>
static uint64_t arrayReduceImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "reduce")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "reduce")) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);

    Rooted<Value> acc{Value::fromUndefined()};
    uint32_t next = 0;
    if (args.count() > 1) {
        acc.set(args[1]);
    } else {
        while (next < len && !rtArrayLikeHasElement(self, Reverse ? len - 1 - next : next)) ++next;
        if (next == len) {
            return rtThrowTypeError("Reduce of empty array with no initial value").rawBits();
        }
        uint32_t firstIdx = Reverse ? len - 1 - next : next;
        acc.set(rtArrayLikeGetElement(self, firstIdx));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        next += 1;
    }

    for (uint32_t n = next; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Value block[4] = {acc.get(), elem.get(), Value::fromDouble(static_cast<double>(i)),
                          self.get()};
        acc.set(Value(bronze_dynamic_call(fn.get().rawBits(), BRONZE_ABI_UNDEFINED_BITS, 4,
                                          reinterpret_cast<const uint64_t*>(block))));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return acc.get().rawBits();
}

uint64_t arrayReduce(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayReduceImpl<false>(env, thisBits, argc, argv);
}

uint64_t arrayReduceRight(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayReduceImpl<true>(env, thisBits, argc, argv);
}

}  // namespace bronze::runtime
