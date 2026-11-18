// An array's EXOTIC elements (array.h kExoticElements): the elements that are
// not plain data values, writable, enumerable and configurable — an accessor
// (`Object.defineProperty(a, i, {get})`), or a data element with an attribute
// of its own (`{writable: false}` on one element of an unfrozen array, `{
// enumerable: false }`). 10.4.2.1 [[DefineOwnProperty]] of an array index is
// OrdinaryDefineOwnProperty, so any of them is a define a program may make.
//
// The block cannot say any of it, so such an element lives where a named
// property of the array does: an entry of the array's dictionary (its shape is
// put in dictionary mode first), under the element's canonical index key, and
// the block holds a HOLE at its index. That is the whole of the design, and
// what keeps the cost off every other array:
//
//  - generated code reads the block and sends a hole to the runtime (a
//    speculated read guards `!is.hole` and leaves); its stores, pushes and
//    pops need the bare array shape, which a dictionary shape is not. Leaving
//    the bare shape is the change an integrity level or a named property
//    makes, and what speculated code already re-tests after any call
//    (opt/core edge_facts: the PlainArray and DenseArray facts read shapes).
//  - the runtime's fast paths read a hole as nothing only while
//    rtArrayChainHoldsIndex is false, which it never is for an array with
//    exotic elements (one bit of its header, read beside the TLS hazard). So
//    every builtin takes its specification walk over such an array, and that
//    walk's hole reads reach rtArrayInheritedHas / Get / Set
//    (rt_array_holes.cpp), which ask this file before the prototype.
//  - the TLS array index hazard is NOT tripped: it is about prototypes, and
//    tripping it would fail every dense-array guard in the program.
//
// The own-key listers (Object.keys, for-in, spread, getOwnPropertyNames) ask
// rtArrayOwnsIndex per index and leave the integer keys out of the named part
// (rt_prop_array.cpp rtArrayOwnNamedKeys); [[GetOwnProperty]] and
// getOwnPropertyDescriptor read the entry as the named property it is.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/accessor.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_leave.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The array's dictionary, when it may hold an exotic element.
Dictionary* exoticTable(Value arr) {
    const ArrayHeader* a = arr.asObject<ArrayHeader>();
    if (!a->hasExoticElements()) return nullptr;
    const Shape* shape = a->object()->shape;
    return shape != nullptr && shape->isDictionary() ? shape->dict : nullptr;
}

// The entry under `index`'s canonical key, found from its digits (no string).
DictEntry* entryAt(Dictionary& d, uint32_t index) {
    char digits[12];
    const auto res = std::to_chars(digits, digits + sizeof(digits), index);
    const auto length = static_cast<uint32_t>(res.ptr - digits);
    return d.findLatin1(digits, length, StringHeader::hashLatin1(digits, length));
}

uint32_t indexOfKey(const StringHeader* key, bool& isIndex) {
    uint32_t index = 0;
    isIndex = key != nullptr && key->isLatin1() &&
              rtIsIntegerLikeKey(std::string_view(key->latin1Data(), key->getLength()), index);
    return index;
}

}  // namespace

bool rtArrayExoticEntry(Value arr, uint32_t index, PropertyInfo& out) {
    Dictionary* d = exoticTable(arr);
    if (d == nullptr) return false;
    const DictEntry* e = entryAt(*d, index);
    if (e == nullptr) return false;
    out.slot = e->slot;
    out.enumerable = e->enumerable;
    out.accessor = e->accessor;
    out.writable = e->writable;
    out.configurable = e->configurable;
    return true;
}

bool rtArrayOwnsIndex(Value arr, uint32_t index, bool enumerableOnly) {
    if (arr.asObject<ArrayHeader>()->hasElem(index)) return true;
    PropertyInfo info;
    return rtArrayExoticEntry(arr, index, info) && (info.enumerable || !enumerableOnly);
}

Value rtArrayExoticGet(Rooted<Value>& arr, uint32_t index, const PropertyInfo& info) {
    (void)index;
    const ObjectHeader* obj = arr.get().asObject<ArrayHeader>()->object();
    if (!info.accessor) return obj->getSlot(info.slot);
    return callGetter(obj->getSlot(info.slot), arr);
}

bool rtArrayExoticSet(Rooted<Value>& arr, uint32_t index, const PropertyInfo& info, Rooted<Value>& val,
                      bool strict) {
    ObjectHeader* obj = arr.get().asObject<ArrayHeader>()->object();
    if (info.accessor) {
        // 10.1.9.2 step 5: the setter with the array as `this`, or the refusal
        // of an accessor with none.
        bool noSetter = false;
        callSetter(obj->getSlot(info.slot + 1), arr, val, &noSetter);
        if (noSetter) rtReportSetRefusal(SetRefusal::NoSetter, strict, std::to_string(index));
        return !noSetter;
    }
    // Step 2.a / 3: the receiver is the holder, so a writable data element is
    // written in place (3.d's [[DefineOwnProperty]] of the value alone).
    if (!info.writable) {
        rtReportSetRefusal(SetRefusal::NotWritable, strict, std::to_string(index));
        return false;
    }
    obj->setSlot(info.slot, val.get());
    return true;
}

bool rtArrayExoticDelete(Value arr, uint32_t index) {
    Dictionary* d = exoticTable(arr);
    DictEntry* e = d != nullptr ? entryAt(*d, index) : nullptr;
    if (e == nullptr) return true;
    if (!e->configurable) return false;
    const uint32_t slot = e->slot;
    const bool accessor = e->accessor;
    d->removeEntry(e);
    ObjectHeader* obj = arr.asObject<ArrayHeader>()->object();
    obj->setSlot(slot, Value::fromUndefined());
    if (accessor) obj->setSlot(slot + 1, Value::fromUndefined());
    return true;
}

uint32_t rtArrayExoticTruncate(Value arr, uint32_t newLen) {
    Dictionary* d = exoticTable(arr);
    if (d == nullptr) return newLen;
    // The indices at or past the new length, highest first (10.4.2.4 step
    // 17.d deletes in descending order and stops at the first refusal).
    std::vector<uint32_t> doomed;
    for (const DictEntry& e : d->entries) {
        if (!e.live()) continue;
        bool isIndex = false;
        const uint32_t index = indexOfKey(e.key.string(), isIndex);
        if (isIndex && index >= newLen) doomed.push_back(index);
    }
    std::sort(doomed.begin(), doomed.end(), [](uint32_t a, uint32_t b) { return a > b; });
    for (const uint32_t index : doomed) {
        if (!rtArrayExoticDelete(arr, index)) return index + 1;
    }
    return newLen;
}

void rtArrayMakeExotic(Rooted<Value>& arr, uint32_t index) {
    {
        runtime::ShapeLeaveReason why(ShapeLeave::Redefine);
        ObjectHeader::toDictionary(rtArena(), arr);
    }
    arr.get().asObject<ArrayHeader>()->reserved |= ArrayHeader::kExoticElements;
    if (!arr.get().asObject<ArrayHeader>()->hasElem(index)) return;
    // The element as it is: its value, enumerable, and the two attributes the
    // array's integrity level gives every element (integrity.h).
    const bool writable = rtArrayElementWriteRefusal(arr.get(), index) == SetRefusal::None;
    const bool configurable = rtArrayElementsConfigurable(arr.get());
    Rooted<Value> value{arr.get().asObject<ArrayHeader>()->getElem(index)};
    char digits[12];
    const auto res = std::to_chars(digits, digits + sizeof(digits), index);
    Rooted<Value> key{rtMakeString(std::string_view(digits, static_cast<size_t>(res.ptr - digits)))};
    uint32_t slot = 0;
    ObjectHeader* live = ObjectHeader::dictDefine(rtHeap(), rtArena(), arr,
                                                  PropertyKey::forString(key.get().asString<StringHeader>()),
                                                  /*enumerable=*/true, /*accessor=*/false, slot);
    live->setSlot(slot, value.get());
    DictEntry* e = entryAt(*arr.get().asObject<ArrayHeader>()->object()->shape->dict, index);
    e->writable = writable;
    e->configurable = configurable;
    arr.get().asObject<ArrayHeader>()->deleteElem(index);
}

}  // namespace bronze::runtime
