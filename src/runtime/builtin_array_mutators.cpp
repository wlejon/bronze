#include "runtime/builtin_array_internal.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"

namespace bronze::runtime {

extern "C" uint64_t bronze_array_push(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Value self(thisBits);
    if (self.isObject() && self.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        ArrayHeader* arr = self.asObject<ArrayHeader>();
        if ((arr->head_offset + arr->length + argc <= arr->capacity) && arr->properties.isUndefined()) {
            // Appends: NUMBERS survives when every value is a Number.
            HeapValue* dst = arr->elementsKeepingNumbers() + arr->length;
            for (uint32_t i = 0; i < argc; ++i) {
                if (!Value(argv[i]).isNumber()) arr->forgetNumbers();
                dst[i] = Value(argv[i]);
            }
            arr->length += argc;
            return Value::fromDouble(arr->length).rawBits();
        }
    }
    return arrayPush(0, thisBits, argc, argv);
}

uint64_t arrayPush(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "push")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    if (isArray(self.get())) {
        if (args.count() > 0 && !requireExtensible(self.get(), "push")) {
            return Value::fromUndefined().rawBits();
        }
        Heap& heap = rtHeap();
        for (uint32_t i = 0; i < args.count(); ++i) {
            ArrayHeader* arr = self.get().asObject<ArrayHeader>();
            arr->setElem(heap, arr->length, args[i]);
        }
        return Value::fromDouble(lengthOf(self.get())).rawBits();
    }
    const uint32_t len = rtArrayLikeLength(self);
    if (static_cast<double>(len) + static_cast<double>(args.count()) > 9007199254740991.0) {
        return rtThrowTypeError("Array.prototype.push: length exceeds maximum").rawBits();
    }
    for (uint32_t i = 0; i < args.count(); ++i) {
        Rooted<Value> val{args[i]};
        rtArrayLikeSetElement(self, len + i, val);
    }
    const uint32_t newLen = len + args.count();
    rtArrayLikeSetLength(self, newLen);
    return Value::fromDouble(newLen).rawBits();
}

extern "C" uint64_t bronze_array_pop(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Value self(thisBits);
    if (self.isObject() && self.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        ArrayHeader* arr = self.asObject<ArrayHeader>();
        // A last element that is a hole is read off the chain (rt_array_holes.cpp)
        // by the full algorithm below, when the chain could answer it.
        if (arr->properties.isUndefined() &&
            (arr->length == 0 || arr->hasElem(arr->length - 1) || !rtArrayChainHoldsIndex(self))) {
            if (arr->length == 0) return Value::fromUndefined().rawBits();
            Value last = arr->getElem(arr->length - 1);
            // The hole lands at the new length: NUMBERS survives.
            arr->elementsKeepingNumbers()[arr->length - 1] = Value::fromHole();
            arr->length -= 1;
            if (arr->length == 0) {
                arr->head_offset = 0;
            }
            return last.rawBits();
        }
    }
    return arrayPop(env, thisBits, argc, argv);
}

uint64_t arrayPop(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{toObject(Value(thisBits), "pop")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    if (isArray(self.get())) {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (arr->length == 0) return Value::fromUndefined().rawBits();
        if (!requireConfigurableElements(self.get(), "pop")) return Value::fromUndefined().rawBits();
        const uint32_t len = arr->length;
        if (!arr->hasElem(len - 1) && rtArrayChainHoldsIndex(self.get())) {
            // Get(len - 1) of a hole is the chain's answer, and it may run a
            // getter — after which the delete and the length are 23.1.3.22's
            // steps on the length read before it, not on whatever is there now.
            Rooted<Value> last{rtArrayInheritedGet(self, len - 1)};
            rtArrayLikeDeleteElement(self, len - 1);
            rtArrayLikeSetLength(self, len - 1);
            return last.get().rawBits();
        }
        Value last = arr->getElem(len - 1);
        // The hole lands at the new length: NUMBERS survives.
        arr->elementsKeepingNumbers()[len - 1] = Value::fromHole();
        arr->length -= 1;
        if (arr->length == 0) {
            arr->head_offset = 0;
        }
        return last.rawBits();
    }
    const uint32_t len = rtArrayLikeLength(self);
    if (len == 0) {
        rtArrayLikeSetLength(self, 0);
        return Value::fromUndefined().rawBits();
    }
    const uint32_t idx = len - 1;
    Rooted<Value> element{rtArrayLikeGetElement(self, idx)};
    rtArrayLikeDeleteElement(self, idx);
    rtArrayLikeSetLength(self, idx);
    return element.get().rawBits();
}

extern "C" uint64_t bronze_array_shift(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Value self(thisBits);
    if (self.isObject() && self.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        ArrayHeader* arr = self.asObject<ArrayHeader>();
        // Moving the block down is the specification's element-by-element walk
        // only when no hole would have read through to the chain.
        if (arr->properties.isUndefined() && !rtArrayHolesSeeChain(self)) {
            if (arr->length == 0) return Value::fromUndefined().rawBits();
            Value first = arr->getElem(0);
            // The hole is left behind the new head: NUMBERS survives.
            arr->elementsKeepingNumbers()[0] = Value::fromHole();
            arr->head_offset += 1;
            arr->length -= 1;
            if (arr->length == 0) {
                arr->head_offset = 0;
            }
            return first.rawBits();
        }
    }
    return arrayShift(env, thisBits, argc, argv);
}

uint64_t arrayShift(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{toObject(Value(thisBits), "shift")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    if (isArray(self.get())) {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (arr->length == 0) return Value::fromUndefined().rawBits();
        if (!requireConfigurableElements(self.get(), "shift")) return Value::fromUndefined().rawBits();
        if (!rtArrayHolesSeeChain(self.get())) {
            Value first = arr->getElem(0);
            // The hole is left behind the new head: NUMBERS survives.
            arr->elementsKeepingNumbers()[0] = Value::fromHole();
            arr->head_offset += 1;
            arr->length -= 1;
            if (arr->length == 0) {
                arr->head_offset = 0;
            }
            return first.rawBits();
        }
        // A hole the chain answers: 23.1.3.27's own walk below, which moves an
        // inherited element down into an own one.
    }
    const uint32_t len = rtArrayLikeLength(self);
    if (len == 0) {
        rtArrayLikeSetLength(self, 0);
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> first{rtArrayLikeGetElement(self, 0)};
    for (uint32_t k = 1; k < len; ++k) {
        uint32_t from = k;
        uint32_t to = k - 1;
        if (rtArrayLikeHasElement(self, from)) {
            Rooted<Value> val{rtArrayLikeGetElement(self, from)};
            rtArrayLikeSetElement(self, to, val);
        } else {
            rtArrayLikeDeleteElement(self, to);
        }
    }
    rtArrayLikeDeleteElement(self, len - 1);
    rtArrayLikeSetLength(self, len - 1);
    return first.get().rawBits();
}

uint64_t arrayUnshift(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "unshift")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    if (isArray(self.get())) {
        const uint32_t n = args.count();
        if (n == 0) return Value::fromDouble(lengthOf(self.get())).rawBits();
        if (!requireExtensible(self.get(), "unshift")) return Value::fromUndefined().rawBits();
    }
    if (isArray(self.get()) && !rtArrayHolesSeeChain(self.get())) {
        const uint32_t n = args.count();
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (arr->head_offset >= n) {
            arr->head_offset -= n;
            arr->length += n;
            HeapValue* data = arr->elementsData();
            for (uint32_t i = 0; i < n; ++i) data[i] = args[i];
            return Value::fromDouble(arr->length).rawBits();
        }

        const uint32_t oldLen = lengthOf(self.get());
        for (uint32_t i = 0; i < n; ++i) {
            Rooted<Value> filler{Value::fromUndefined()};
            appendTo(self, filler);
        }
        arr = self.get().asObject<ArrayHeader>();
        HeapValue* data = arr->elementsData();
        for (uint32_t i = oldLen; i > 0; --i) data[i - 1 + n] = data[i - 1];
        for (uint32_t i = 0; i < n; ++i) data[i] = args[i];
        return Value::fromDouble(arr->length).rawBits();
    }
    const uint32_t len = rtArrayLikeLength(self);
    const uint32_t n = args.count();
    if (n > 0) {
        if (static_cast<double>(len) + static_cast<double>(n) > 9007199254740991.0) {
            return rtThrowTypeError("Array.prototype.unshift: length exceeds maximum").rawBits();
        }
        for (uint32_t k = len; k > 0; --k) {
            uint32_t from = k - 1;
            uint32_t to = from + n;
            if (rtArrayLikeHasElement(self, from)) {
                Rooted<Value> val{rtArrayLikeGetElement(self, from)};
                rtArrayLikeSetElement(self, to, val);
            } else {
                rtArrayLikeDeleteElement(self, to);
            }
        }
        for (uint32_t j = 0; j < n; ++j) {
            Rooted<Value> val{args[j]};
            rtArrayLikeSetElement(self, j, val);
        }
    }
    const uint32_t newLen = len + n;
    rtArrayLikeSetLength(self, newLen);
    return Value::fromDouble(newLen).rawBits();
}

uint64_t arrayReverse(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{toObject(Value(thisBits), "reverse")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    if (isArray(self.get())) {
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        if (arr->length > 1 && !requireWritableElements(self.get(), "reverse")) {
            return Value::fromUndefined().rawBits();
        }
        if (!rtArrayHolesSeeChain(self.get())) {
            HeapValue* data = arr->elementsData();
            for (uint32_t i = 0, j = arr->length; i + 1 < j; ++i, --j) {
                std::swap(data[i], data[j - 1]);
            }
            return self.get().rawBits();
        }
    }
    const uint32_t len = rtArrayLikeLength(self);
    const uint32_t middle = len / 2;
    for (uint32_t lower = 0; lower < middle; ++lower) {
        uint32_t upper = len - lower - 1;
        bool lowerExists = rtArrayLikeHasElement(self, lower);
        Rooted<Value> lowerVal{lowerExists ? rtArrayLikeGetElement(self, lower) : Value::fromUndefined()};

        bool upperExists = rtArrayLikeHasElement(self, upper);
        Rooted<Value> upperVal{upperExists ? rtArrayLikeGetElement(self, upper) : Value::fromUndefined()};

        if (lowerExists && upperExists) {
            rtArrayLikeSetElement(self, lower, upperVal);
            rtArrayLikeSetElement(self, upper, lowerVal);
        } else if (!lowerExists && upperExists) {
            rtArrayLikeSetElement(self, lower, upperVal);
            rtArrayLikeDeleteElement(self, upper);
        } else if (lowerExists && !upperExists) {
            rtArrayLikeDeleteElement(self, lower);
            rtArrayLikeSetElement(self, upper, lowerVal);
        }
    }
    return self.get().rawBits();
}

uint64_t arrayFill(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "fill")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? self.get().asObject<ArrayHeader>()->length
                                             : rtArrayLikeLength(self);
    uint32_t start = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    uint32_t end = args.count() > 2 && !args[2].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[2])), len)
                       : len;
    if (isArray(self.get()) && self.get().asObject<ArrayHeader>()->length == len &&
        self.get().asObject<ArrayHeader>()->head_offset + len <=
            self.get().asObject<ArrayHeader>()->capacity &&
        (len == 0 || self.get().asObject<ArrayHeader>()->elementsData() != nullptr)) {
        if (start < end && !requireWritableElements(self.get(), "fill")) {
            return Value::fromUndefined().rawBits();
        }
        const Value fillVal = args[0];
        HeapValue* data = self.get().asObject<ArrayHeader>()->elementsData();
        for (uint32_t i = start; i < end; ++i) data[i] = fillVal;
        return self.get().rawBits();
    }
    Rooted<Value> fillVal{args[0]};
    for (uint32_t i = start; i < end; ++i) {
        rtArrayLikeSetElement(self, i, fillVal);
    }
    return self.get().rawBits();
}

uint64_t arraySplice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "splice")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    const uint32_t start =
        args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t deleteCount = 0;
    if (args.count() == 1) {
        deleteCount = len - start;
    } else if (args.count() > 1) {
        double dc = toInteger(rtToNumber(args[1]));
        if (dc < 0) dc = 0;
        const double most = static_cast<double>(len - start);
        deleteCount = static_cast<uint32_t>(dc < most ? dc : most);
    }
    const uint32_t insertCount = args.count() > 2 ? args.count() - 2 : 0;
    if (static_cast<double>(len) + static_cast<double>(insertCount) - static_cast<double>(deleteCount) > 9007199254740991.0) {
        return rtThrowTypeError("Array.prototype.splice: length exceeds maximum").rawBits();
    }

    Rooted<Value> removed{rtArraySpeciesCreate(self, 0)};

    if (isArray(self.get()) && isArray(removed.get()) && !rtArrayHolesSeeChain(self.get()) &&
        self.get().asObject<ArrayHeader>()->length == len &&
        self.get().asObject<ArrayHeader>()->head_offset + len <=
            self.get().asObject<ArrayHeader>()->capacity &&
        (len == 0 || self.get().asObject<ArrayHeader>()->elementsData() != nullptr)) {
        const uint32_t newLen = len - deleteCount + insertCount;
        const uint32_t moveCount = len - start - deleteCount;

        for (uint32_t i = 0; i < deleteCount; ++i) {
            if (!hasIndex(self.get(), start + i)) {
                appendHole(removed);
                continue;
            }
            Rooted<Value> elem{elemOf(self.get(), start + i)};
            appendTo(removed, elem);
        }

        if (newLen > len) {
            if (!requireExtensible(self.get(), "splice")) return Value::fromUndefined().rawBits();
            for (uint32_t i = len; i < newLen; ++i) {
                Rooted<Value> filler{Value::fromUndefined()};
                appendTo(self, filler);
            }
            ArrayHeader* arr = self.get().asObject<ArrayHeader>();
            HeapValue* data = arr->elementsData();
            for (uint32_t i = moveCount; i > 0; --i) {
                data[start + insertCount + i - 1] = data[start + deleteCount + i - 1];
            }
        } else if (newLen < len) {
            if (moveCount > 0 && !requireWritableElements(self.get(), "splice")) {
                return Value::fromUndefined().rawBits();
            }
            ArrayHeader* arr = self.get().asObject<ArrayHeader>();
            HeapValue* data = arr->elementsData();
            for (uint32_t i = 0; i < moveCount; ++i) {
                data[start + insertCount + i] = data[start + deleteCount + i];
            }
            if (!requireConfigurableElements(self.get(), "splice")) {
                return Value::fromUndefined().rawBits();
            }
            arr->length = newLen;
        } else if (insertCount > 0) {
            if (!requireWritableElements(self.get(), "splice")) {
                return Value::fromUndefined().rawBits();
            }
        }

        if (insertCount > 0) {
            HeapValue* data = self.get().asObject<ArrayHeader>()->elementsData();
            for (uint32_t i = 0; i < insertCount; ++i) data[start + i] = args[i + 2];
        }
        return removed.get().rawBits();
    }

    // Generic splice path:
    for (uint32_t i = 0; i < deleteCount; ++i) {
        uint32_t from = start + i;
        if (rtArrayLikeHasElement(self, from)) {
            Rooted<Value> fromVal{rtArrayLikeGetElement(self, from)};
            rtCreateDataPropertyOrThrow(removed, i, fromVal);
        }
    }
    rtArrayLikeSetLength(removed, deleteCount);

    if (insertCount < deleteCount) {
        for (uint32_t k = start; k < len - deleteCount; ++k) {
            uint32_t from = k + deleteCount;
            uint32_t to = k + insertCount;
            if (rtArrayLikeHasElement(self, from)) {
                Rooted<Value> fromVal{rtArrayLikeGetElement(self, from)};
                rtArrayLikeSetElement(self, to, fromVal);
            } else {
                rtArrayLikeDeleteElement(self, to);
            }
        }
        for (uint32_t k = len; k > len - deleteCount + insertCount; --k) {
            rtArrayLikeDeleteElement(self, k - 1);
        }
    } else if (insertCount > deleteCount) {
        for (uint32_t k = len - deleteCount; k > start; --k) {
            uint32_t from = k + deleteCount - 1;
            uint32_t to = k + insertCount - 1;
            if (rtArrayLikeHasElement(self, from)) {
                Rooted<Value> fromVal{rtArrayLikeGetElement(self, from)};
                rtArrayLikeSetElement(self, to, fromVal);
            } else {
                rtArrayLikeDeleteElement(self, to);
            }
        }
    }
    for (uint32_t i = 0; i < insertCount; ++i) {
        Rooted<Value> item{args[i + 2]};
        rtArrayLikeSetElement(self, start + i, item);
    }
    rtArrayLikeSetLength(self, len - deleteCount + insertCount);
    return removed.get().rawBits();
}

uint64_t arrayCopyWithin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "copyWithin")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? self.get().asObject<ArrayHeader>()->length
                                             : rtArrayLikeLength(self);

    uint32_t to = args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t from = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    uint32_t final = args.count() > 2 && !args[2].isUndefined()
                         ? relativeIndex(toInteger(rtToNumber(args[2])), len)
                         : len;

    uint32_t count = final > from ? std::min(final - from, len - to) : 0;
    if (count == 0) return self.get().rawBits();

    if (isArray(self.get()) && !rtArrayHolesSeeChain(self.get()) &&
        self.get().asObject<ArrayHeader>()->length == len &&
        self.get().asObject<ArrayHeader>()->head_offset + len <=
            self.get().asObject<ArrayHeader>()->capacity &&
        self.get().asObject<ArrayHeader>()->elementsData() != nullptr) {
        if (!requireWritableElements(self.get(), "copyWithin")) {
            return Value::fromUndefined().rawBits();
        }

        HeapValue* data = self.get().asObject<ArrayHeader>()->elementsData();
        if (from < to && to < from + count) {
            for (uint32_t i = count; i > 0; --i) {
                data[to + i - 1] = data[from + i - 1];
            }
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                data[to + i] = data[from + i];
            }
        }
        return self.get().rawBits();
    }

    int32_t direction = 1;
    if (from < to && to < from + count) {
        direction = -1;
        from = from + count - 1;
        to = to + count - 1;
    }
    while (count > 0) {
        if (rtArrayLikeHasElement(self, from)) {
            Rooted<Value> fromVal{rtArrayLikeGetElement(self, from)};
            rtArrayLikeSetElement(self, to, fromVal);
        } else {
            rtArrayLikeDeleteElement(self, to);
        }
        from += direction;
        to += direction;
        --count;
    }
    return self.get().rawBits();
}

}  // namespace bronze::runtime
