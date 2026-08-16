// `Array.prototype.sort` (ECMA-262 23.1.3.30) — stable, and shaped exactly as
// the clause is: the elements are READ into a list first
// (SortIndexedProperties, skip-holes), the LIST is sorted, and only a sort
// that finished writes anything back. That order is not an optimisation — it
// is the answer to a comparator that mutates the array mid-sort (the sort saw
// the snapshot; the mutations are overwritten by the write-back) and to one
// that throws (the array is untouched, because no write had happened yet).
//
// The list lives in an ordinary bronze ARRAY rather than a std::vector of
// Values, and that is the GC story in one decision: a comparator is user code,
// so every call into it can collect, and a C++ vector of raw Values would be a
// buffer of pre-collection addresses by the second comparison. A bronze array
// held through one Rooted is scanned by the collector like any other object,
// so the sort re-derives its element block through the root after every
// comparator call and never holds a raw pointer across one.
//
// The comparator itself is 23.1.3.30.2 CompareArrayElements: both-undefined is
// equal, undefined sorts AFTER everything, a user comparator's answer goes
// through ToNumber with NaN as 0, and the default is ToString on both sides
// and a code-unit comparison — which is why [10, 9, 1] sorts to [1, 10, 9].

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

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

// 23.1.3.30.2 CompareArrayElements. -1 / 0 / +1; a pending exception is the
// fourth answer, and every caller tests the cell before using the number.
int compareElements(Rooted<Value>& x, Rooted<Value>& y, Rooted<Value>& comparefn) {
    const bool xu = x.get().isUndefined();
    const bool yu = y.get().isUndefined();
    // Steps 1-3: undefined sorts to the END, and two undefineds do not move
    // relative to each other — the comparator is never consulted about one.
    if (xu && yu) return 0;
    if (xu) return 1;
    if (yu) return -1;
    if (!comparefn.get().isUndefined()) {
        Value block[2] = {x.get(), y.get()};
        Rooted<Value> answer{Value(bronze_dynamic_call(comparefn.get().rawBits(),
                                                       BRONZE_ABI_UNDEFINED_BITS, 2,
                                                       reinterpret_cast<const uint64_t*>(block)))};
        if (rtExceptionPending()) return 0;
        // Step 4.b-4.d: ToNumber, with NaN read as "equal" rather than as an
        // ordering — which is what keeps a garbage comparator from making the
        // sort loop forever.
        const double v = rtToNumber(answer.get());
        if (v < 0) return -1;
        if (v > 0) return 1;
        return 0;
    }
    // Steps 5-9: ToString on BOTH (through ToPrimitive, so an object element
    // runs its own toString and can throw), then a code-unit comparison.
    Rooted<Value> xs{rtToStringValue(x)};
    if (rtExceptionPending()) return 0;
    Rooted<Value> ys{rtToStringValue(y)};
    if (rtExceptionPending()) return 0;
    const StringHeader* a = xs.get().asString<StringHeader>();
    const StringHeader* b = ys.get().asString<StringHeader>();
    if (a->lessThan(*b)) return -1;
    if (b->lessThan(*a)) return 1;
    return 0;
}

// One merge of src[lo, mid) and src[mid, hi) into dst[lo, hi). Stability is
// the `<= 0` below: on a tie the LEFT run's element goes first, and the left
// run is the one that came first in the input.
void mergeRuns(Rooted<Value>& src, Rooted<Value>& dst, uint32_t lo, uint32_t mid, uint32_t hi,
               Rooted<Value>& comparefn) {
    uint32_t i = lo;
    uint32_t j = mid;
    for (uint32_t k = lo; k < hi; ++k) {
        bool takeLeft;
        if (i >= mid) {
            takeLeft = false;
        } else if (j >= hi) {
            takeLeft = true;
        } else {
            Rooted<Value> a{getAt(src, i)};
            Rooted<Value> b{getAt(src, j)};
            takeLeft = compareElements(a, b, comparefn) <= 0;
            if (rtExceptionPending()) return;
        }
        setAt(dst, k, getAt(src, takeLeft ? i++ : j++));
    }
}

}  // namespace

uint64_t rtArraySortBuiltin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!isArray(self.get())) {
        return rtThrowTypeError(
                   "Array.prototype.sort called on a value that is not an array")
            .rawBits();
    }
    // Step 1: the comparator is validated BEFORE anything is read, so a bad
    // one does not leave a half-snapshot behind.
    Rooted<Value> comparefn{args[0]};
    if (!comparefn.get().isUndefined() && !isCallable(comparefn.get())) {
        return rtThrowTypeError("The comparison function must be either a function or undefined")
            .rawBits();
    }

    const uint32_t len = self.get().asObject<ArrayHeader>()->length;

    // SortIndexedProperties with SKIP-HOLES: what is sorted is the elements
    // that are THERE. A hole is not an undefined — undefined is in the list
    // and sorts to its end; a hole is absent from it and reappears after the
    // items, as the deletes at the bottom.
    Rooted<Value> list{Value(bronze_create_array(0))};
    for (uint32_t i = 0; i < len; ++i) {
        if (!self.get().asObject<ArrayHeader>()->hasElem(i)) continue;
        const uint32_t at = list.get().asObject<ArrayHeader>()->length;
        setAt(list, at, getAt(self, i));
    }
    const uint32_t itemCount = list.get().asObject<ArrayHeader>()->length;

    // Bottom-up merge sort between the list and a scratch array of the same
    // length: O(n log n), stable, and no recursion for a comparator to blow
    // the C++ stack through.
    Rooted<Value> scratch{Value(bronze_create_array(0))};
    for (uint32_t i = 0; i < itemCount; ++i) setAt(scratch, i, Value::fromUndefined());

    Rooted<Value>* src = &list;
    Rooted<Value>* dst = &scratch;
    for (uint32_t width = 1; width < itemCount; width *= 2) {
        for (uint32_t lo = 0; lo < itemCount; lo += 2 * width) {
            const uint32_t mid = lo + width < itemCount ? lo + width : itemCount;
            const uint32_t hi = lo + 2 * width < itemCount ? lo + 2 * width : itemCount;
            mergeRuns(*src, *dst, lo, mid, hi, comparefn);
            // A comparator that threw ends the SORT, not just the merge — and
            // the receiver has not been written, so the array the catch sees
            // is the array the program had.
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        }
        Rooted<Value>* t = src;
        src = dst;
        dst = t;
    }
    Rooted<Value>& sorted = *src;

    // Steps 7-9, the write-back — refused BEFORE the first mutation, which is
    // where the spec refuses it too: every write below is a Set with throw
    // semantics, so on a frozen array the FIRST one throws with nothing yet
    // moved, and on a non-extensible array the first hole it would fill does.
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
        setAt(self, i, getAt(sorted, i));
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
    return self.get().rawBits();  // sorts IN PLACE and answers the same array
}

}  // namespace bronze::runtime
