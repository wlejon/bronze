// `Array.prototype.sort` (ECMA-262 23.1.3.30) — stable, and shaped exactly as
// the clause is: the elements are READ into a list first
// (SortIndexedProperties, skip-holes), the LIST is sorted, and only a sort
// that finished writes anything back. That order is not an optimisation — it
// is the answer to a comparator that mutates the array mid-sort (the sort saw
// the snapshot; the mutations are overwritten by the write-back) and to one
// that throws (the array is untouched, because no write had happened yet).
//
// The list is snapshotted into an ordinary bronze ARRAY and sorted in a
// RootedBlock rather than a std::vector of Values, and that is the GC story:
// a comparator is user code, so every call into it can collect, and a C++
// vector of raw Values would be a buffer of pre-collection addresses by the
// second comparison. Every slot of the block is a root the collector updates
// in place, so the merge reads its slots after every comparator call and never
// holds a Value across one.
//
// The comparator itself is 23.1.3.30.2 CompareArrayElements: both-undefined is
// equal, undefined sorts AFTER everything, a user comparator's answer goes
// through ToNumber with NaN as 0, and the default is ToString on both sides
// and a code-unit comparison — which is why [10, 9, 1] sorts to [1, 10, 9].

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_array_internal.h"
#include "runtime/call_out.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Element access through a ROOT, because everything in this file runs between
// comparator calls that can move the array. `setAt` writes an index that
// already exists, so it cannot grow the block — but `setElem` is used anyway,
// through the root, so the invariant is the callee's and not this comment's.
Value getAt(Rooted<Value>& arr, uint32_t i) {
    return arr.get().asObject<ArrayHeader>()->getElem(i);
}

void setAt(Rooted<Value>& arr, uint32_t i, Value v) {
    Rooted<Value> val{v};
    arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, val);
}

// ---- the merge engine --------------------------------------------------------
//
// three.js sorts its render list with a user comparator every frame — a
// 5,000-element list, ~61,000 comparisons — so what the engine does around a
// comparison is per-frame cost. The merge runs over ONE RootedBlock holding the
// list and its scratch, two halves of `itemCount` slots: every slot is a root,
// so the collector updates the Values in place and a raw `Value*` into the
// block stays valid and current across a comparator call. That makes the
// merge plain loads and stores — no element block to re-derive through a root
// after each call, no write barrier per store (a root is not a heap slot), no
// rooted temporaries per comparison. Every read of a slot happens AFTER the
// comparison it follows, so no Value is held across a window where user code
// runs (the comparator, and a ToNumber/ToString that reaches a valueOf).
struct SortContext {
    SortContext(Rooted<Value>& fn, const DirectCallee& callee) : comparefn(fn), direct(callee) {}
    Rooted<Value>& comparefn;
    const DirectCallee& direct;
    // The default comparison's operands and the comparator's answer, rooted
    // once for the whole sort: ToString and ToNumber can run user code.
    Rooted<Value> a;
    Rooted<Value> b;
    Rooted<Value> answer;
};

// 23.1.3.30.2 CompareArrayElements. -1 / 0 / +1; a comparator's throw
// propagates through the sort, before any write-back has begun.
int compareElements(SortContext& c, Value x, Value y) {
    const bool xu = x.isUndefined();
    const bool yu = y.isUndefined();
    // Steps 1-3: undefined sorts to the END, and two undefineds do not move
    // relative to each other — the comparator is never consulted about one.
    if (xu && yu) return 0;
    if (xu) return 1;
    if (yu) return -1;
    if (!c.comparefn.get().isUndefined()) {
        // Filled from the block on the statement before the call, and the
        // call reaches the callee without allocating, so nothing moves in
        // between (rt_roots.h says when a block must be rooted instead).
        const uint64_t block[2] = {x.rawBits(), y.rawBits()};
        // The comparator was bound ONCE, before the first merge: what
        // `bronze_dynamic_call` would re-derive per comparison — object,
        // function rather than proxy, arity reached — is a fact about a value
        // the sort holds and nothing can change under it. `call_out.h` says
        // what the binding may and may not cache.
        const Value ans = c.direct.bound()
                              ? Value(c.direct.call(c.comparefn, BRONZE_ABI_UNDEFINED_BITS, 2, block))
                              : Value(bronze_dynamic_call(c.comparefn.get().rawBits(),
                                                          BRONZE_ABI_UNDEFINED_BITS, 2, block));
        // Step 4.b-4.d: ToNumber, with NaN read as "equal" rather than as an
        // ordering — which is what keeps a garbage comparator from making the
        // sort loop forever. A number answer — the whole population for a
        // well-typed comparator — is its own ToNumber; anything else keeps the
        // spec'd conversion, through the root, because a valueOf can run and
        // allocate.
        double v;
        if (ans.isNumber()) {
            v = ans.asNumber();
        } else {
            c.answer = ans;
            v = rtToNumber(c.answer.get());
        }
        if (v < 0) return -1;
        if (v > 0) return 1;
        return 0;
    }
    // Steps 5-9: ToString on BOTH (through ToPrimitive, so an object element
    // runs its own toString and can throw), then a code-unit comparison.
    c.a = x;
    c.b = y;
    Rooted<Value> xs{rtToStringValue(c.a)};
    Rooted<Value> ys{rtToStringValue(c.b)};
    const StringHeader* as = xs.get().asString<StringHeader>();
    const StringHeader* bs = ys.get().asString<StringHeader>();
    if (as->lessThan(*bs)) return -1;
    if (bs->lessThan(*as)) return 1;
    return 0;
}

// One merge of src[lo, mid) and src[mid, hi) into dst[lo, hi). Stability is
// the `<= 0` below: on a tie the LEFT run's element goes first, and the left
// run is the one that came first in the input. `src` and `dst` point into the
// rooted block, so their slots are re-read after every comparison.
void mergeRuns(SortContext& c, Value* src, Value* dst, uint32_t lo, uint32_t mid, uint32_t hi) {
    uint32_t i = lo;
    uint32_t j = mid;
    uint32_t k = lo;
    while (i < mid && j < hi) {
        const bool takeLeft = compareElements(c, src[i], src[j]) <= 0;
        dst[k++] = takeLeft ? src[i++] : src[j++];
    }
    while (i < mid) dst[k++] = src[i++];
    while (j < hi) dst[k++] = src[j++];
}

}  // namespace

uint64_t rtArraySortBuiltin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 1: the comparator is validated BEFORE anything is read, so a bad
    // one does not leave a half-snapshot behind.
    Rooted<Value> comparefn{args[0]};
    if (!comparefn.get().isUndefined() && !isCallable(comparefn.get())) {
        return rtThrowTypeError("The comparison function must be either a function or undefined")
            .rawBits();
    }
    Rooted<Value> self{toObject(Value(thisBits), "sort")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();

    const bool isArr = isArray(self.get());
    const uint32_t len = isArr ? self.get().asObject<ArrayHeader>()->length : rtArrayLikeLength(self);

    // SortIndexedProperties with SKIP-HOLES: what is sorted is the elements
    // that are THERE. A hole is not an undefined — undefined is in the list
    // and sorts to its end; a hole is absent from it and reappears after the
    // items, as the deletes at the bottom.
    //
    // The snapshot is presized to `len`, so the appends below never grow it.
    Rooted<Value> list{Value::fromObject(ArrayHeader::create(rtHeap(), len > 4 ? len : 4))};
    // A hole the chain answers is an item (HasProperty), which the generic
    // read takes.
    if (isArr && !rtArrayChainHoldsIndex(self.get())) {
        for (uint32_t i = 0; i < len; ++i) {
            if (!self.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            const uint32_t at = list.get().asObject<ArrayHeader>()->length;
            setAt(list, at, getAt(self, i));
        }
    } else {
        for (uint32_t i = 0; i < len; ++i) {
            if (!rtArrayLikeHasElement(self, i)) continue;
            Rooted<Value> val{rtArrayLikeGetElement(self, i)};
            const uint32_t at = list.get().asObject<ArrayHeader>()->length;
            setAt(list, at, val.get());
        }
    }
    const uint32_t itemCount = list.get().asObject<ArrayHeader>()->length;

    // Bottom-up merge sort between the two halves of one rooted block (the
    // merge engine above says why a block): O(n log n), stable, and no
    // recursion for a comparator to blow the C++ stack through.
    RootedBlock work(itemCount * 2);
    Value* items = work.slots();
    for (uint32_t i = 0; i < itemCount; ++i) items[i] = getAt(list, i);

    // Bound once for the whole sort, and only once: a comparator cannot turn
    // into something else between two comparisons, so the guard ladder
    // bronze_dynamic_call runs per call has one answer here.
    DirectCallee direct;
    direct.bind(comparefn.get(), 2);
    // One scope for the whole sort, where the generic call path pushed and
    // popped one per comparison. Every user-code call this sort makes — the
    // comparator, and any `valueOf`/`toString` that ToNumber of its answer
    // reaches — is a PLAIN call, and 13.3.12 makes new.target `undefined` in
    // all of them, so one scope covering all of them says what the language
    // says. Before this, the window BETWEEN two comparator calls reported the
    // enclosing constructor's new.target to a `valueOf` the constructor never
    // constructed.
    NewTargetScope sortTargetScope(Value::fromUndefined());
    SortContext ctx(comparefn, direct);

    Value* src = items;
    Value* dst = items + itemCount;
    for (uint32_t width = 1; width < itemCount; width *= 2) {
        for (uint32_t lo = 0; lo < itemCount; lo += 2 * width) {
            const uint32_t mid = lo + width < itemCount ? lo + width : itemCount;
            const uint32_t hi = lo + 2 * width < itemCount ? lo + 2 * width : itemCount;
            // A comparator that threw ends the SORT, not just the merge — and
            // the receiver has not been written, so the array the catch sees
            // is the array the program had.
            mergeRuns(ctx, src, dst, lo, mid, hi);
        }
        Value* t = src;
        src = dst;
        dst = t;
    }
    const Value* sorted = src;

    // Steps 7-9, the write-back — refused BEFORE the first mutation, which is
    // where the spec refuses it too: every write below is a Set with throw
    // semantics, so on a frozen array the FIRST one throws with nothing yet
    // moved, and on a non-extensible array the first hole it would fill does.
    if (isArr) {
        for (uint32_t i = 0; i < itemCount; ++i) {
            const SetRefusal refusal = rtArrayElementWriteRefusal(self.get(), i);
            if (refusal == SetRefusal::NotWritable) {
                return rtThrowTypeError("Cannot assign to read only element of a frozen array "
                                        "(Array.prototype.sort)")
                    .rawBits();
            }
            if (refusal != SetRefusal::None) {
                return rtThrowTypeError("Cannot add elements to an array that is not extensible "
                                        "(Array.prototype.sort)")
                    .rawBits();
            }
            // A hole written back is a Set the chain may take (a setter at
            // that index), which the full write asks.
            if (!self.get().asObject<ArrayHeader>()->hasElem(i) && rtArrayChainHoldsIndex(self.get())) {
                rtArrayIndexSet(self.get(), i, sorted[i], /*strict=*/true);
                continue;
            }
            setAt(self, i, sorted[i]);
        }
        // Step 9: the tail past the items is DELETED, which is what moves the
        // holes after the undefineds. DeletePropertyOrThrow, so a sealed array
        // with a hole in it refuses here by name.
        for (uint32_t i = itemCount; i < len; ++i) {
            if (!self.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            if (!rtArrayElementsConfigurable(self.get())) {
                return rtThrowTypeError("Cannot delete property " + std::to_string(i) +
                                        " of a sealed array (Array.prototype.sort)")
                    .rawBits();
            }
            self.get().asObject<ArrayHeader>()->deleteElem(i);
        }
    } else {
        for (uint32_t i = 0; i < itemCount; ++i) {
            Rooted<Value> val{sorted[i]};
            rtArrayLikeSetElement(self, i, val);
        }
        for (uint32_t i = itemCount; i < len; ++i) {
            rtArrayLikeDeleteElement(self, i);
        }
    }
    return self.get().rawBits();  // sorts IN PLACE and answers the same array
}

}  // namespace bronze::runtime
