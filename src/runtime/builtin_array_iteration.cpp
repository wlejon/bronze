#include "runtime/builtin_array_internal.h"
#include "runtime/call_out.h"
#include "runtime/rt_roots.h"
#include "runtime/tls_block.h"
#include "abi/bronze_abi.h"

namespace bronze::runtime {

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
        next += 1;
    }

    for (uint32_t n = next; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        Value block[4] = {acc.get(), elem.get(), Value::fromDouble(static_cast<double>(i)),
                          self.get()};
        acc.set(Value(bronze_dynamic_call(fn.get().rawBits(), BRONZE_ABI_UNDEFINED_BITS, 4,
                                          reinterpret_cast<const uint64_t*>(block))));
    }
    return acc.get().rawBits();
}

uint64_t arrayReduceRight(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayReduceImpl<true>(env, thisBits, argc, argv);
}

}  // namespace bronze::runtime
