// The JS surface of `ArrayBuffer` and the nine views: the constructor objects,
// the four construction paths of 23.2.5.1, and the members an instance answers.
// The METHODS live next door in builtin_typed_array_methods.cpp; the
// representation lives in typed_array.{h,cpp}.

#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

Value fromLength(ElementKind kind, uint32_t length) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    if (!checkAllocatable(length * bpe)) return Value::fromUndefined();
    return Value::fromObject(TypedArrayHeader::create(rtHeap(), kind, length));
}

Value fromBuffer(ElementKind kind, Rooted<Value>& buffer, Value offsetVal, Value lengthVal) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    // 23.2.5.1 runs ToIndex on the offset before it even looks at the length,
    // and ToIndex is ToNumber: `new Int32Array(buf, {valueOf(){…}}, {valueOf(){…}})`
    // runs user code between the two reads, so the length argument needs a root
    // of its own — the caller's copy is a local the collector cannot update.
    Rooted<Value> offsetRoot{offsetVal};
    Rooted<Value> lengthRoot{lengthVal};
    uint32_t offset = 0;
    if (!toIndex(offsetRoot.get(), "byte offset", 1, offset)) return Value::fromUndefined();
    if (offset % bpe != 0) {
        rtThrowRangeError("start offset of " + std::string(elementKindInfo(kind).name) +
                          " should be a multiple of " + std::to_string(bpe));
        return Value::fromUndefined();
    }

    // 23.2.5.1 -> InitializeTypedArrayFromArrayBuffer runs ToIndex(length)
    // BEFORE it tests the buffer (its steps 4..6), so both conversions'
    // `valueOf`s have run by the time the buffer is measured — a length whose
    // conversion detaches or resizes it is judged against the buffer as it is
    // NOW, not as it was. That is why the detach test and the byteLength read
    // sit below the conversion and re-derive through the root.
    const bool hasLength = !lengthRoot.get().isUndefined();
    uint32_t length = 0;
    if (hasLength && !toIndex(lengthRoot.get(), "typed array", bpe, length)) {
        return Value::fromUndefined();
    }
    auto* buf = buffer.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        rtThrowTypeError("ArrayBuffer is detached");
        return Value::fromUndefined();
    }
    const uint32_t bufferLength = buf->byteLength;
    if (offset > bufferLength) {
        rtThrowRangeError("Start offset " + std::to_string(offset) +
                          " is outside the bounds of the buffer");
        return Value::fromUndefined();
    }

    if (!hasLength) {
        // No length argument: over a resizable buffer (a growable
        // SharedArrayBuffer included) that is 10.4.5's length-TRACKING view —
        // [[ArrayLength]] is auto, recomputed by every resize, and there is
        // no divisibility condition on the tail (the length floors instead).
        // Over a fixed buffer the rest of the bytes must divide evenly and
        // the count is fixed here, once.
        if (buf->isResizable()) {
            return Value::fromObject(TypedArrayHeader::createOverBuffer(
                rtHeap(), kind, buffer, offset, (bufferLength - offset) / bpe,
                /*tracking=*/true));
        }
        if ((bufferLength - offset) % bpe != 0) {
            rtThrowRangeError("byte length of " + std::string(elementKindInfo(kind).name) +
                              " should be a multiple of " + std::to_string(bpe));
            return Value::fromUndefined();
        }
        length = (bufferLength - offset) / bpe;
    } else if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(length) * bpe >
               bufferLength) {
        rtThrowRangeError("Invalid typed array length: " + std::to_string(length));
        return Value::fromUndefined();
    }
    return Value::fromObject(
        TypedArrayHeader::createOverBuffer(rtHeap(), kind, buffer, offset, length));
}

Value fromTypedArray(ElementKind kind, Rooted<Value>& source) {
    const ElementKind srcKind = source.get().asObject<TypedArrayHeader>()->elementKind();
    // 23.2.5.1 step 5 -> InitializeTypedArrayFromTypedArray step 5: mixing a
    // BigInt view with a Number one is a TypeError, because there is no
    // conversion between the two content types at all (23.2.5.13 goes through
    // ToBigInt and 23.2.5.14 through ToNumber, and neither accepts the other's
    // values).
    if (isBigIntElementKind(kind) != isBigIntElementKind(srcKind)) {
        return rtThrowTypeError(std::string("Cannot construct a ") +
                                elementKindInfo(kind).name + " from a " +
                                elementKindInfo(srcKind).name +
                                " (one holds BigInts and the other Numbers)");
    }
    const uint32_t length = source.get().asObject<TypedArrayHeader>()->length;
    Rooted<Value> out{fromLength(kind, length)};
    if (rtExceptionPending()) return Value::fromUndefined();
    for (uint32_t i = 0; i < length; ++i) {
        if (isBigIntElementKind(kind)) {
            // Both views are 8 bytes wide and the stored bits are the same 64
            // whichever signedness each has, so this is a copy and never a
            // conversion — which is also why it cannot allocate.
            const uint64_t bits = source.get().asObject<TypedArrayHeader>()->rawBits64(i);
            out.get().asObject<TypedArrayHeader>()->setRawBits64(i, bits);
            continue;
        }
        const double v = source.get().asObject<TypedArrayHeader>()->get(i);
        out.get().asObject<TypedArrayHeader>()->set(i, v);
    }
    return out.get();
}

Value fromArrayLike(ElementKind kind, Rooted<Value>& source) {
    if (isArray(source.get())) {
        const uint32_t length = source.get().asObject<ArrayHeader>()->length;
        Rooted<Value> out{fromLength(kind, length)};
        if (rtExceptionPending()) return Value::fromUndefined();
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
            // The store's conversion is the element kind's, so a Number in a
            // BigInt view's source array is the TypeError 7.1.13 names rather
            // than a truncation.
            rtTypedArraySetElement(out, i, elem.get());
            if (rtExceptionPending()) return Value::fromUndefined();
        }
        return out.get();
    }

    // 23.2.5.1 step 5: usingIterator is GetMethod(object, @@iterator), and when
    // it is undefined the constructor falls to step 5.c —
    // InitializeTypedArrayFromArrayLike, which reads `length` and the indices
    // and never asks for an iterator at all. Without this arm every object
    // without @@iterator was "not iterable", which is a TypeError the language
    // does not raise: `new Float64Array({length: 2, 0: 1.5, 1: 2.5})` is a
    // two-element view, and an object with no `length` at all is a length-0
    // one rather than an error.
    if (!rtHasIteratorMethod(source)) {
        const uint32_t length = rtArrayLikeLength(source);
        if (rtExceptionPending()) return Value::fromUndefined();
        Rooted<Value> out{fromLength(kind, length)};
        if (rtExceptionPending()) return Value::fromUndefined();
        for (uint32_t i = 0; i < length; ++i) {
            // Both the element read and the ToNumber under it can run user
            // code, so the view is reached through its root each time rather
            // than through a pointer taken before the loop.
            Rooted<Value> elem{rtArrayLikeElement(source, i)};
            if (rtExceptionPending()) return Value::fromUndefined();
            rtTypedArraySetElement(out, i, elem.get());
            if (rtExceptionPending()) return Value::fromUndefined();
        }
        return out.get();
    }

    Rooted<Value> collected{Value(bronze_create_array(0))};
    Rooted<Value> rec{Value(bronze_iter_open(source.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        bronze_array_append(collected.get().rawBits(), item.get().rawBits());
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) {
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        return Value::fromUndefined();
    }
    return fromArrayLike(kind, collected);
}

Value constructTypedArray(ElementKind kind, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> arg{args[0]};

    if (arg.get().isUndefined()) return fromLength(kind, 0);
    if (arg.get().isNumber()) {
        uint32_t length = 0;
        if (!toIndex(arg.get(), "typed array", elementKindInfo(kind).bytesPerElement, length)) {
            return Value::fromUndefined();
        }
        return fromLength(kind, length);
    }
    if (isBuffer(arg.get())) return fromBuffer(kind, arg, args[1], args[2]);
    if (arg.get().isObject()) {
        const uint16_t flags = arg.get().asObject<HeapObjectHeader>()->flags;
        if (flags == TypedArrayHeader::kFlags) return fromTypedArray(kind, arg);
        if (flags == HeapKind::Function) {
            return rtThrowTypeError(std::string(elementKindInfo(kind).name) +
                                    " constructor: a function is not iterable");
        }
        return fromArrayLike(kind, arg);
    }
    return rtThrowTypeError(std::string(elementKindInfo(kind).name) +
                            " constructor requires a length, a buffer, an array or an iterable");
}

template <ElementKind K>
uint64_t typedArrayCtor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return constructTypedArray(K, argc, argv).rawBits();
}

uint64_t arrayBufferCtor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    uint32_t byteLength = 0;
    if (!toIndex(args[0], "array buffer", 1, byteLength)) return Value::fromUndefined().rawBits();
    if (!checkAllocatable(byteLength)) return Value::fromUndefined().rawBits();

    if (args.count() > 1 && args[1].isObject()) {
        Rooted<Value> opts{args[1]};
        Rooted<Value> mblKey{rtMakeString("maxByteLength")};
        Value mblVal =
            opts.get().asObject<ObjectHeader>()->getProp(rtHeap(), mblKey, nullptr, opts.slot_ptr());
        if (!mblVal.isUndefined()) {
            uint32_t maxByteLength = 0;
            if (!toIndex(mblVal, "maxByteLength", 1, maxByteLength)) {
                return Value::fromUndefined().rawBits();
            }
            if (maxByteLength < byteLength) {
                return rtThrowRangeError("maxByteLength must be >= byteLength").rawBits();
            }
            if (!checkAllocatable(maxByteLength)) return Value::fromUndefined().rawBits();
            return Value::fromObject(
                       ArrayBufferHeader::createResizable(rtHeap(), byteLength, maxByteLength))
                .rawBits();
        }
    }
    return Value::fromObject(ArrayBufferHeader::create(rtHeap(), byteLength)).rawBits();
}

struct CtorEntry {
    ElementKind kind;
    bronze_fn_code code;
};

const CtorEntry kCtors[] = {
    {ElementKind::Int8, typedArrayCtor<ElementKind::Int8>},
    {ElementKind::Uint8, typedArrayCtor<ElementKind::Uint8>},
    {ElementKind::Uint8Clamped, typedArrayCtor<ElementKind::Uint8Clamped>},
    {ElementKind::Int16, typedArrayCtor<ElementKind::Int16>},
    {ElementKind::Uint16, typedArrayCtor<ElementKind::Uint16>},
    {ElementKind::Int32, typedArrayCtor<ElementKind::Int32>},
    {ElementKind::Uint32, typedArrayCtor<ElementKind::Uint32>},
    {ElementKind::Float32, typedArrayCtor<ElementKind::Float32>},
    {ElementKind::Float64, typedArrayCtor<ElementKind::Float64>},
    {ElementKind::Float16, typedArrayCtor<ElementKind::Float16>},
    {ElementKind::BigInt64, typedArrayCtor<ElementKind::BigInt64>},
    {ElementKind::BigUint64, typedArrayCtor<ElementKind::BigUint64>},
};

static_assert(std::size(kCtors) == static_cast<size_t>(ElementKind::Count),
              "the constructor table has drifted from the ElementKind enum");

const char* const kTypedArrayUnimplemented[] = {
    "toLocaleString",
};

const char* const kTypedArraySlotMembers[] = {
    "length", "byteLength", "byteOffset", "buffer", "BYTES_PER_ELEMENT", "constructor",
};

// 25.1.5.5 ArrayBuffer.prototype.resize
uint64_t arrayBufferResize(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!isBuffer(self)) {
        return rtThrowTypeError("ArrayBuffer.prototype.resize called on non-ArrayBuffer").rawBits();
    }
    auto* buf = self.asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("Cannot resize a detached ArrayBuffer").rawBits();
    }
    if (!buf->isResizable()) {
        return rtThrowTypeError("Cannot resize a non-resizable ArrayBuffer").rawBits();
    }
    uint32_t newLen = 0;
    if (!toIndex(args[0], "byte length", 1, newLen)) return Value::fromUndefined().rawBits();
    if (newLen > buf->maxByteLength) {
        return rtThrowRangeError("Invalid byte length: exceeds maxByteLength").rawBits();
    }
    if (newLen > buf->byteLength) {
        std::memset(buf->data() + buf->byteLength, 0, newLen - buf->byteLength);
    }
    buf->byteLength = newLen;
    // A shrink strands the views that no longer fit; a grow can re-admit
    // them. Their length fields carry the truth, so they are re-derived here,
    // at the mutation, rather than checked on every element access.
    Rooted<Value> selfRoot{self};
    closeOrReopenViews(rtHeap(), selfRoot);
    return Value::fromUndefined().rawBits();
}

// 25.1.5.7 ArrayBuffer.prototype.transfer
uint64_t arrayBufferTransfer(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!isBuffer(self.get())) {
        return rtThrowTypeError("ArrayBuffer.prototype.transfer called on non-ArrayBuffer").rawBits();
    }
    auto* buf = self.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("Cannot transfer a detached ArrayBuffer").rawBits();
    }
    uint32_t newLen = buf->byteLength;
    if (args.count() > 0 && !args[0].isUndefined()) {
        if (!toIndex(args[0], "byte length", 1, newLen)) return Value::fromUndefined().rawBits();
    }
    if (!checkAllocatable(newLen)) return Value::fromUndefined().rawBits();

    const bool resizable = buf->isResizable();
    const uint32_t maxByteLen = buf->maxByteLength;
    const uint32_t oldLen = buf->byteLength;
    if (resizable && newLen > maxByteLen) {
        return rtThrowRangeError("newByteLength exceeds maxByteLength").rawBits();
    }

    Rooted<Value> newBufVal{Value::fromUndefined()};
    if (resizable) {
        newBufVal.set(Value::fromObject(
            ArrayBufferHeader::createResizable(rtHeap(), newLen, maxByteLen)));
    } else {
        newBufVal.set(Value::fromObject(
            ArrayBufferHeader::create(rtHeap(), newLen)));
    }
    auto* oldBuf = self.get().asObject<ArrayBufferHeader>();
    auto* newBuf = newBufVal.get().asObject<ArrayBufferHeader>();
    const uint32_t copyLen = std::min(oldLen, newLen);
    std::memcpy(newBuf->data(), oldBuf->data(), copyLen);
    oldBuf->setDetached();
    // The detach closes every view over the old buffer FOREVER (its
    // byteLength is 0 from here on); their length fields carry the truth,
    // so they are zeroed here, at the mutation, rather than checked on every
    // element access.
    closeOrReopenViews(rtHeap(), self);
    return newBufVal.get().rawBits();
}

// 25.1.5.8 ArrayBuffer.prototype.transferToFixedLength
uint64_t arrayBufferTransferToFixedLength(uint64_t, uint64_t thisBits, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!isBuffer(self.get())) {
        return rtThrowTypeError("ArrayBuffer.prototype.transferToFixedLength called on non-ArrayBuffer").rawBits();
    }
    auto* buf = self.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("Cannot transfer a detached ArrayBuffer").rawBits();
    }
    uint32_t newLen = buf->byteLength;
    if (args.count() > 0 && !args[0].isUndefined()) {
        if (!toIndex(args[0], "byte length", 1, newLen)) return Value::fromUndefined().rawBits();
    }
    if (!checkAllocatable(newLen)) return Value::fromUndefined().rawBits();

    const uint32_t oldLen = buf->byteLength;
    Rooted<Value> newBufVal{Value::fromObject(ArrayBufferHeader::create(rtHeap(), newLen))};
    auto* oldBuf = self.get().asObject<ArrayBufferHeader>();
    auto* newBuf = newBufVal.get().asObject<ArrayBufferHeader>();
    const uint32_t copyLen = std::min(oldLen, newLen);
    std::memcpy(newBuf->data(), oldBuf->data(), copyLen);
    oldBuf->setDetached();
    closeOrReopenViews(rtHeap(), self);
    return newBufVal.get().rawBits();
}

// 25.1.5.6 ArrayBuffer.prototype.slice
uint64_t arrayBufferSlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!isBuffer(self.get())) {
        return rtThrowTypeError("ArrayBuffer.prototype.slice called on non-ArrayBuffer").rawBits();
    }
    auto* buf = self.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("Cannot slice a detached ArrayBuffer").rawBits();
    }
    const uint32_t len = buf->byteLength;
    uint32_t first = 0;
    if (args.count() > 0 && !args[0].isUndefined()) {
        first = relativeIndex(toInteger(rtToNumber(args[0])), len);
    }
    uint32_t final = len;
    if (args.count() > 1 && !args[1].isUndefined()) {
        final = relativeIndex(toInteger(rtToNumber(args[1])), len);
    }
    const uint32_t newLen = final > first ? final - first : 0;
    Rooted<Value> newBufVal{Value::fromObject(ArrayBufferHeader::create(rtHeap(), newLen))};
    auto* oldBuf = self.get().asObject<ArrayBufferHeader>();
    auto* newBuf = newBufVal.get().asObject<ArrayBufferHeader>();
    if (newLen > 0) {
        std::memcpy(newBuf->data(), oldBuf->data() + first, newLen);
    }
    return newBufVal.get().rawBits();
}

// %TypedArray%.from
uint64_t typedArrayFrom(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value ctorVal(thisBits);
    ElementKind kind = ElementKind::Float64;
    bool found = false;
    for (const CtorEntry& entry : kCtors) {
        if (ctorVal.isObject() && ctorVal.asObject<FunctionHeader>()->code == entry.code) {
            kind = entry.kind;
            found = true;
            break;
        }
    }
    if (!found) {
        return rtThrowTypeError("%TypedArray%.from called on non-TypedArray constructor").rawBits();
    }
    Rooted<Value> source{args[0]};
    Rooted<Value> mapFn{args[1]};
    Rooted<Value> thisArg{args[2]};
    const bool hasMap = !mapFn.get().isUndefined();
    if (hasMap && !isCallable(mapFn.get())) {
        return rtThrowTypeError("mapFn is not callable").rawBits();
    }

    if (isTypedArray(source.get())) {
        const uint32_t len = lengthOf(source.get());
        Rooted<Value> out{fromLength(kind, len)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        for (uint32_t i = 0; i < len; ++i) {
            Rooted<Value> val{rtTypedArrayElement(source.get(), i)};
            if (hasMap) {
                val.set(callBack(mapFn, thisArg, val, i, source));
                if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            }
            rtTypedArraySetElement(out, i, val.get());
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        }
        return out.get().rawBits();
    }

    if (isArray(source.get())) {
        const uint32_t len = source.get().asObject<ArrayHeader>()->length;
        Rooted<Value> out{fromLength(kind, len)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        for (uint32_t i = 0; i < len; ++i) {
            Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
            if (hasMap) {
                elem.set(callBack(mapFn, thisArg, elem, i, source));
                if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            }
            rtTypedArraySetElement(out, i, elem.get());
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        }
        return out.get().rawBits();
    }

    Rooted<Value> collected{Value(bronze_create_array(0))};
    Rooted<Value> rec{Value(bronze_iter_open(source.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        bronze_array_append(collected.get().rawBits(), item.get().rawBits());
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) {
        bronze_iter_close(rec.get().rawBits(), true);
        return Value::fromUndefined().rawBits();
    }
    const uint64_t block[3] = {collected.get().rawBits(), mapFn.get().rawBits(),
                               thisArg.get().rawBits()};
    return typedArrayFrom(0, thisBits, 3, block);
}

// %TypedArray%.of
uint64_t typedArrayOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value ctorVal(thisBits);
    ElementKind kind = ElementKind::Float64;
    bool found = false;
    for (const CtorEntry& entry : kCtors) {
        if (ctorVal.isObject() && ctorVal.asObject<FunctionHeader>()->code == entry.code) {
            kind = entry.kind;
            found = true;
            break;
        }
    }
    if (!found) {
        return rtThrowTypeError("%TypedArray%.of called on non-TypedArray constructor").rawBits();
    }
    Rooted<Value> out{fromLength(kind, args.count())};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    for (uint32_t i = 0; i < args.count(); ++i) {
        rtTypedArraySetElement(out, i, args[i]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return out.get().rawBits();
}

}  // namespace

Value rtTypedArrayConstructor(const std::string& name) {
    if (name == "ArrayBuffer") return rtNativeFunction(arrayBufferCtor, 0);
    for (const CtorEntry& entry : kCtors) {
        if (name == elementKindInfo(entry.kind).name) {
            return rtNativeFunction(entry.code, 0);
        }
    }
    return Value::fromUndefined();
}

const char* rtTypedArrayConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == arrayBufferCtor) return "ArrayBuffer";
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code) return elementKindInfo(entry.kind).name;
    }
    return nullptr;
}

Value rtTypedArrayConstructorFor(ElementKind kind) {
    for (const CtorEntry& entry : kCtors) {
        if (entry.kind == kind) return rtNativeFunction(entry.code, 0);
    }
    fatal("internal: no constructor for this typed-array element kind");
}

bool rtTypedArrayStatic(Value fn, const std::string& key, Value& out) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != code) continue;
        if (key == "BYTES_PER_ELEMENT") {
            out = Value::fromDouble(elementKindInfo(entry.kind).bytesPerElement);
            return true;
        }
        if (key == "from") {
            out = rtNativeFunction(typedArrayFrom, 1);
            return true;
        }
        if (key == "of") {
            out = rtNativeFunction(typedArrayOf, 0);
            return true;
        }
        return false;
    }
    return false;
}

// One element as a JS VALUE, which is where the two BigInt views stop being
// "two more widths": ten kinds answer a Number and these two answer a BigInt, so
// every read path in the runtime funnels through here rather than each one
// keeping its own opinion about `view->get`.
//
// ALLOCATES for a BigInt kind, and the bytes are therefore read BEFORE the
// allocation — the raw `view` pointer is dead from that line on. Out of range is
// `undefined`, which 10.4.5.4 makes an absence rather than an error.
Value rtTypedArrayElement(Value viewVal, uint32_t index) {
    auto* view = viewVal.asObject<TypedArrayHeader>();
    if (index >= view->length) return Value::fromUndefined();
    const ElementKind kind = view->elementKind();
    if (!isBigIntElementKind(kind)) return Value::fromDouble(view->get(index));
    const uint64_t bits = view->rawBits64(index);
    return rtBigIntFromRawBits64(bits, kind == ElementKind::BigInt64);
}

// 10.4.5.16 IntegerIndexedElementSet. The conversion is ToNumber for the ten
// numeric kinds and ToBigInt for the two 64-bit integer ones — 23.2.5.13's
// split, and the one place in the language where a typed-array write THROWS
// (7.1.13 has no Number row) instead of truncating.
//
// Either conversion can run user code, so the view is re-derived through the
// root afterwards and its length re-read — the conversion itself may have
// transferred the buffer away, which zeroes the window. An index that is out
// of range by then is a discarded write, not an error; the
// conversion-before-validity order is 10.4.5.16's own.
void rtTypedArraySetElement(Rooted<Value>& view, uint32_t index, Value value) {
    const ElementKind kind = view.get().asObject<TypedArrayHeader>()->elementKind();
    Rooted<Value> val{value};
    if (isBigIntElementKind(kind)) {
        uint64_t bits = 0;
        if (!rtBigIntToRawBits64(val.get(), bits)) return;
        auto* live = view.get().asObject<TypedArrayHeader>();
        if (index < live->length) live->setRawBits64(index, bits);
        return;
    }
    const double num = rtToNumber(val.get());
    if (rtExceptionPending()) return;
    auto* live = view.get().asObject<TypedArrayHeader>();
    if (index < live->length) live->set(index, num);
}

Value rtTypedArrayMember(Value viewVal, const std::string& key) {
    auto* view = viewVal.asObject<TypedArrayHeader>();
    // `length` and `byteLength` answer 0 for a view its buffer left behind
    // (23.2.4.2–.3) through the maintained window alone; `byteOffset` still
    // has to ask (23.2.4.4 answers +0 out of bounds, and a closed view's
    // stored offset survives the closing). `buffer` below always answers —
    // the identity outlives the window.
    if (key == "length") return Value::fromDouble(view->length);
    if (key == "byteLength") return Value::fromDouble(view->byteLength());
    if (key == "byteOffset") {
        return Value::fromDouble(view->isOutOfBounds() ? 0.0 : view->byteOffset);
    }
    if (key == "buffer") return view->buffer;
    if (key == "BYTES_PER_ELEMENT") return Value::fromDouble(view->bytesPerElement());
    if (key == "constructor") return rtTypedArrayConstructorFor(view->elementKind());

    const char* kindName = view->kindName();
    // 23.2.3's methods over a BIGINT view are not built. Every one of them in
    // this runtime speaks `double` — the loops hold a raw view pointer and a
    // numeric element — and a 64-bit integer element is neither representable
    // as one nor convertible to one without losing the low bits. So they are
    // refused BY NAME here rather than answered with an approximation, which is
    // the silent wrong answer the project's rules exist to prevent.
    //
    // What IS built for these two views is the whole of what makes them
    // different: construction from every source 23.2.5.1 lists, `from` and
    // `of`, element reads and writes with ToBigInt, iteration, and printing.
    if (isBigIntElementKind(view->elementKind()) && rtTypedArrayHasMethod(key)) {
        fatal((std::string("unsupported: ") + kindName + ".prototype." + key +
               " is not implemented (bronze's %TypedArray% methods convert every element "
               "through a double, which cannot carry a 64-bit integer; the two BigInt views "
               "support construction, element access, iteration and `from`/`of`)")
                  .c_str());
    }
    Value method = rtTypedArrayMethod(key);
    if (!method.isUndefined()) return method;
    rtCheckTypedArrayMember(kindName, key);
    return Value::fromUndefined();
}

void rtCheckTypedArrayMember(const char* kindName, const std::string& key) {
    const std::string receiver = std::string(kindName) + ".prototype";
    rtCheckUnimplementedMember(receiver.c_str(), kTypedArrayUnimplemented,
                               std::size(kTypedArrayUnimplemented), key);
}

Value rtArrayBufferMember(Value bufferVal, const std::string& key) {
    auto* buf = bufferVal.asObject<ArrayBufferHeader>();
    // One kind, two surfaces: 25.2's SharedArrayBuffer members are a different
    // set from 25.1's and live with the rest of the shared-memory surface
    // (builtin_shared_memory.cpp). Delegated here rather than branched at the
    // property path so `in`, the reads and the printer cannot disagree.
    if (buf->isShared()) return rtSharedArrayBufferMember(bufferVal, key);
    if (key == "byteLength") {
        return Value::fromDouble(buf->isDetached() ? 0.0 : static_cast<double>(buf->byteLength));
    }
    if (key == "maxByteLength") {
        return Value::fromDouble(buf->isDetached() ? 0.0
                                                   : static_cast<double>(buf->maxByteLength));
    }
    if (key == "resizable") {
        return Value::fromBool(buf->isDetached() ? false : buf->isResizable());
    }
    if (key == "detached") {
        return Value::fromBool(buf->isDetached());
    }
    if (key == "resize") return rtNativeFunction(arrayBufferResize, 1);
    if (key == "transfer") return rtNativeFunction(arrayBufferTransfer, 0);
    if (key == "transferToFixedLength") {
        return rtNativeFunction(arrayBufferTransferToFixedLength, 0);
    }
    if (key == "slice") return rtNativeFunction(arrayBufferSlice, 0);
    if (key == "constructor") return rtTypedArrayConstructor("ArrayBuffer");
    return Value::fromUndefined();
}

bool rtTypedArrayHasMember(const char* kindName, const std::string& key) {
    for (const char* name : kTypedArraySlotMembers) {
        if (key == name) return true;
    }
    if (rtTypedArrayHasMethod(key)) return true;
    rtCheckTypedArrayMember(kindName, key);
    return false;
}

bool rtArrayBufferHasMember(bool shared, const std::string& key) {
    if (shared) return rtSharedArrayBufferHasMember(key);
    if (key == "byteLength" || key == "maxByteLength" || key == "resizable" ||
        key == "detached" || key == "resize" || key == "transfer" ||
        key == "transferToFixedLength" || key == "slice" || key == "constructor") {
        return true;
    }
    return false;
}

}  // namespace bronze::runtime
