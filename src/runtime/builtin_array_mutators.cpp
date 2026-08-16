#include "runtime/builtin_array_internal.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"

namespace bronze::runtime {

extern "C" uint64_t bronze_array_push(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Value self(thisBits);
    if (self.isObject() && self.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        ArrayHeader* arr = self.asObject<ArrayHeader>();
        if (argc == 1 && (arr->head_offset + arr->length < arr->capacity) && arr->properties.isUndefined()) {
            arr->elementsData()[arr->length++] = Value(argv[0]);
            return Value::fromDouble(arr->length).rawBits();
        }
    }
    return arrayPush(0, thisBits, argc, argv);
}

uint64_t arrayPush(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "push")) return Value::fromUndefined().rawBits();
    if (args.count() > 0 && !requireExtensible(self.get(), "push")) {
        return Value::fromUndefined().rawBits();
    }
    for (uint32_t i = 0; i < args.count(); ++i) {
        Rooted<Value> v{args[i]};
        appendTo(self, v);
    }
    return Value::fromDouble(lengthOf(self.get())).rawBits();
}

uint64_t arrayPop(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireArray(self, "pop")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    if (arr->length == 0) return Value::fromUndefined().rawBits();
    if (!requireConfigurableElements(self, "pop")) return Value::fromUndefined().rawBits();
    Value last = arr->getElem(arr->length - 1);
    arr->elementsData()[arr->length - 1] = Value::fromHole();
    arr->length -= 1;
    if (arr->length == 0) {
        arr->head_offset = 0;
    }
    return last.rawBits();
}

uint64_t arrayShift(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireArray(self, "shift")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    if (arr->length == 0) return Value::fromUndefined().rawBits();
    if (!requireConfigurableElements(self, "shift")) return Value::fromUndefined().rawBits();
    Value first = arr->getElem(0);
    arr->elementsData()[0] = Value::fromHole();
    arr->head_offset += 1;
    arr->length -= 1;
    if (arr->length == 0) {
        arr->head_offset = 0;
    }
    return first.rawBits();
}

uint64_t arrayUnshift(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "unshift")) return Value::fromUndefined().rawBits();
    const uint32_t n = args.count();
    if (n == 0) return Value::fromDouble(lengthOf(self.get())).rawBits();
    if (!requireExtensible(self.get(), "unshift")) return Value::fromUndefined().rawBits();

    ArrayHeader* arr = self.get().asObject<ArrayHeader>();
    if (arr->head_offset >= n) {
        arr->head_offset -= n;
        arr->length += n;
        Value* data = arr->elementsData();
        for (uint32_t i = 0; i < n; ++i) data[i] = args[i];
        return Value::fromDouble(arr->length).rawBits();
    }

    const uint32_t oldLen = lengthOf(self.get());
    for (uint32_t i = 0; i < n; ++i) {
        Rooted<Value> filler{Value::fromUndefined()};
        appendTo(self, filler);
    }
    arr = self.get().asObject<ArrayHeader>();
    Value* data = arr->elementsData();
    for (uint32_t i = oldLen; i > 0; --i) data[i - 1 + n] = data[i - 1];
    for (uint32_t i = 0; i < n; ++i) data[i] = args[i];
    return Value::fromDouble(arr->length).rawBits();
}

uint64_t arrayReverse(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireArray(self, "reverse")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    if (arr->length > 1 && !requireWritableElements(self, "reverse")) {
        return Value::fromUndefined().rawBits();
    }
    Value* data = arr->elementsData();
    for (uint32_t i = 0, j = arr->length; i + 1 < j; ++i, --j) {
        std::swap(data[i], data[j - 1]);
    }
    return self.rawBits();
}

uint64_t arrayFill(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Rooted, and the header taken only AFTER the index conversions: ToNumber
    // of an object argument is ToPrimitive, so `a.fill(0, {valueOf(){...}})`
    // runs user code and can move the array. The length is a copy and survives;
    // the pointer would not.
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "fill")) return Value::fromUndefined().rawBits();
    const uint32_t len = self.get().asObject<ArrayHeader>()->length;
    uint32_t start = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    uint32_t end = args.count() > 2 && !args[2].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[2])), len)
                       : len;
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (start < end && !requireWritableElements(self.get(), "fill")) {
        return Value::fromUndefined().rawBits();
    }
    const Value fillVal = args[0];
    Value* data = self.get().asObject<ArrayHeader>()->elementsData();
    for (uint32_t i = start; i < end; ++i) data[i] = fillVal;
    return self.get().rawBits();
}

uint64_t arraySplice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "splice")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
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
    const uint32_t newLen = len - deleteCount + insertCount;
    const uint32_t moveCount = len - start - deleteCount;

    Rooted<Value> removed{newArray()};
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
        Value* data = arr->elementsData();
        for (uint32_t i = moveCount; i > 0; --i) {
            data[start + insertCount + i - 1] = data[start + deleteCount + i - 1];
        }
    } else if (newLen < len) {
        if (moveCount > 0 && !requireWritableElements(self.get(), "splice")) {
            return Value::fromUndefined().rawBits();
        }
        ArrayHeader* arr = self.get().asObject<ArrayHeader>();
        Value* data = arr->elementsData();
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
        Value* data = self.get().asObject<ArrayHeader>()->elementsData();
        for (uint32_t i = 0; i < insertCount; ++i) data[start + i] = args[i + 2];
    }
    return removed.get().rawBits();
}

// 23.1.3.4 Array.prototype.copyWithin(target, start, end = this.length)
uint64_t arrayCopyWithin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Rooted for the reason `fill` is: the three index conversions can each be
    // a user `valueOf`, and the elements pointer is taken after all of them.
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "copyWithin")) return Value::fromUndefined().rawBits();
    const uint32_t len = self.get().asObject<ArrayHeader>()->length;

    uint32_t to = args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t from = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    uint32_t final = args.count() > 2 && !args[2].isUndefined()
                         ? relativeIndex(toInteger(rtToNumber(args[2])), len)
                         : len;
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    uint32_t count = final > from ? std::min(final - from, len - to) : 0;
    if (count == 0) return self.get().rawBits();

    if (!requireWritableElements(self.get(), "copyWithin")) {
        return Value::fromUndefined().rawBits();
    }

    Value* data = self.get().asObject<ArrayHeader>()->elementsData();
    if (from < to && to < from + count) {
        // Copy backwards to handle overlap
        for (uint32_t i = count; i > 0; --i) {
            data[to + i - 1] = data[from + i - 1];
        }
    } else {
        // Copy forwards
        for (uint32_t i = 0; i < count; ++i) {
            data[to + i] = data[from + i];
        }
    }
    return self.get().rawBits();
}

}  // namespace bronze::runtime
