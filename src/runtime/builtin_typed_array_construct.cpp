// Constructing a typed array (ECMA-262 23.2.5.1) and the two statics that
// construct one (23.2.2.1 `from`, 23.2.2.2 `of`). The constructor objects,
// the prototypes and the accessors are builtin_typed_array.cpp; the methods
// are builtin_typed_array_methods.cpp; the representation is
// typed_array.{h,cpp}.
//
// 23.2.5.1 step 4 is AllocateTypedArray over NewTarget, and bronze performs it
// at the one allocation site every construction goes through
// (`rtAllocateNativeBaseInstance`, runtime/native_base.h) — so every path
// below receives a view that ALREADY EXISTS, uninitialized, with the
// [[Prototype]] 10.1.14 derived, and fills its [[ViewedArrayBuffer]],
// [[ByteOffset]] and [[ArrayLength]] in place. That is what makes
// `class V extends Float32Array` work: the derived constructor's `this` is
// that object, `super(n)` is the body below run on it, and the class fields
// initialize on it afterwards.

#include <cmath>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 23.2.5.1.1 AllocateTypedArrayBuffer: a fresh zero-filled `%ArrayBuffer%` —
// the intrinsic, never a species — of `length` elements, initializing the
// handed view over it. False with the RangeError pending when it does not fit.
bool fillFromLength(Rooted<Value>& view, ElementKind kind, uint32_t length) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    if (!checkAllocatable(length * bpe)) return false;
    Rooted<Value> buffer{rtNewArrayBuffer(length * bpe)};
    view.get().asObject<TypedArrayHeader>()->initialize(buffer, 0, length, /*tracking=*/false);
    return true;
}

// 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer.
bool fillFromBuffer(Rooted<Value>& view, ElementKind kind, Rooted<Value>& buffer, Value offsetVal,
                    Value lengthVal) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    // ToIndex on the offset runs before the length is even looked at, and
    // ToIndex is ToNumber: `new Int32Array(buf, {valueOf(){…}}, {valueOf(){…}})`
    // runs user code between the two reads, so the length argument needs a
    // root of its own — the caller's copy is a local the collector cannot
    // update.
    Rooted<Value> offsetRoot{offsetVal};
    Rooted<Value> lengthRoot{lengthVal};
    uint32_t offset = 0;
    if (!toIndex(offsetRoot.get(), "byte offset", 1, offset)) return false;
    if (offset % bpe != 0) {
        rtThrowRangeError("start offset of " + std::string(elementKindInfo(kind).name) +
                          " should be a multiple of " + std::to_string(bpe));
        return false;
    }

    // ToIndex(length) runs BEFORE the buffer is tested (steps 4..6), so both
    // conversions' `valueOf`s have run by the time the buffer is measured — a
    // length whose conversion detaches or resizes it is judged against the
    // buffer as it is NOW. That is why the detach test and the byteLength
    // read sit below the conversion and re-derive through the root.
    const bool hasLength = !lengthRoot.get().isUndefined();
    uint32_t length = 0;
    if (hasLength && !toIndex(lengthRoot.get(), "typed array", bpe, length)) return false;
    auto* buf = buffer.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        rtThrowTypeError("ArrayBuffer is detached");
        return false;
    }
    const uint32_t bufferLength = buf->byteLength;
    if (offset > bufferLength) {
        rtThrowRangeError("Start offset " + std::to_string(offset) +
                          " is outside the bounds of the buffer");
        return false;
    }

    if (!hasLength) {
        // No length argument: over a resizable buffer (a growable
        // SharedArrayBuffer included) that is 10.4.5's length-TRACKING view —
        // [[ArrayLength]] is auto, recomputed by every resize, and there is
        // no divisibility condition on the tail (the length floors instead).
        // Over a fixed buffer the rest of the bytes must divide evenly and
        // the count is fixed here, once.
        if (buf->isResizable()) {
            view.get().asObject<TypedArrayHeader>()->initialize(
                buffer, offset, (bufferLength - offset) / bpe, /*tracking=*/true);
            return true;
        }
        if ((bufferLength - offset) % bpe != 0) {
            rtThrowRangeError("byte length of " + std::string(elementKindInfo(kind).name) +
                              " should be a multiple of " + std::to_string(bpe));
            return false;
        }
        length = (bufferLength - offset) / bpe;
    } else if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(length) * bpe >
               bufferLength) {
        rtThrowRangeError("Invalid typed array length: " + std::to_string(length));
        return false;
    }
    view.get().asObject<TypedArrayHeader>()->initialize(buffer, offset, length,
                                                        /*tracking=*/false);
    return true;
}

// 23.2.5.1.2 InitializeTypedArrayFromTypedArray.
bool fillFromTypedArray(Rooted<Value>& view, ElementKind kind, Rooted<Value>& source) {
    const ElementKind srcKind = kindOf(source.get());
    // Step 5: mixing a BigInt view with a Number one is a TypeError, because
    // there is no conversion between the two content types at all (23.2.5.13
    // goes through ToBigInt and 23.2.5.14 through ToNumber, and neither
    // accepts the other's values).
    if (isBigIntElementKind(kind) != isBigIntElementKind(srcKind)) {
        rtThrowTypeError(std::string("Cannot construct a ") + elementKindInfo(kind).name +
                         " from a " + elementKindInfo(srcKind).name +
                         " (one holds BigInts and the other Numbers)");
        return false;
    }
    const uint32_t length = lengthOf(source.get());
    if (!fillFromLength(view, kind, length)) return false;
    for (uint32_t i = 0; i < length; ++i) {
        auto* src = source.get().asObject<TypedArrayHeader>();
        auto* dst = view.get().asObject<TypedArrayHeader>();
        if (isBigIntElementKind(kind)) {
            // Both views are 8 bytes wide and the stored bits are the same 64
            // whichever signedness each has, so this is a copy and never a
            // conversion — which is also why it cannot allocate.
            dst->setRawBits64(i, src->rawBits64(i));
            continue;
        }
        dst->set(i, src->get(i));
    }
    return true;
}

// 23.2.5.1.4 InitializeTypedArrayFromList and 23.2.5.1.5
// InitializeTypedArrayFromArrayLike.
bool fillFromArrayLike(Rooted<Value>& view, ElementKind kind, Rooted<Value>& source) {
    if (isArray(source.get())) {
        const uint32_t length = source.get().asObject<ArrayHeader>()->length;
        if (!fillFromLength(view, kind, length)) return false;
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
            // The store's conversion is the element kind's, so a Number in a
            // BigInt view's source array is the TypeError 7.1.13 names rather
            // than a truncation.
            rtTypedArraySetElement(view, i, elem.get());
            if (rtExceptionPending()) return false;
        }
        return true;
    }

    // 23.2.5.1 step 5: usingIterator is GetMethod(object, @@iterator), and when
    // it is undefined the constructor falls to step 5.c —
    // InitializeTypedArrayFromArrayLike, which reads `length` and the indices
    // and never asks for an iterator at all: `new Float64Array({length: 2, 0:
    // 1.5, 1: 2.5})` is a two-element view, and an object with no `length` at
    // all is a length-0 one rather than an error.
    if (!rtHasIteratorMethod(source)) {
        const uint32_t length = rtArrayLikeLength(source);
        if (rtExceptionPending()) return false;
        if (!fillFromLength(view, kind, length)) return false;
        for (uint32_t i = 0; i < length; ++i) {
            // Both the element read and the ToNumber under it can run user
            // code, so the view is reached through its root each time rather
            // than through a pointer taken before the loop.
            Rooted<Value> elem{rtArrayLikeElement(source, i)};
            if (rtExceptionPending()) return false;
            rtTypedArraySetElement(view, i, elem.get());
            if (rtExceptionPending()) return false;
        }
        return true;
    }

    Rooted<Value> collected{Value(bronze_create_array(0))};
    Rooted<Value> rec{Value(bronze_iter_open(source.get().rawBits()))};
    if (rtExceptionPending()) return false;
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        bronze_array_append(collected.get().rawBits(), item.get().rawBits());
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) {
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        return false;
    }
    return fillFromArrayLike(view, kind, collected);
}

// The constructor `C` a static or a species algorithm was handed, as the view
// kind it builds — when it IS one of the twelve intrinsics. Their instances
// need no `new`: the intrinsic's own instance shape is what NewTarget would
// have derived, and nothing a program can observe differs.
bool intrinsicViewKind(Value ctor, ElementKind& out) {
    return rtTypedArrayConstructorKind(ctor, out);
}

}  // namespace

Value rtTypedArrayConstructBody(ElementKind kind, Rooted<Value>& receiver, uint32_t argc,
                                const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // 23.2.5.1 step 1: NewTarget undefined is a TypeError. The uniform calling
    // convention offers one witness — the receiver is the object the running
    // construction allocated — and the brand is asked as well, so that
    // `Reflect.construct(Uint8Array, [], Float32Array)` fills a Uint8Array and
    // never a Float32Array.
    const char* name = elementKindInfo(kind).name;
    if (!rtIsNativeConstructReceiver(receiver.get()) || !isTypedArray(receiver.get()) ||
        kindOf(receiver.get()) != kind ||
        receiver.get().asObject<TypedArrayHeader>()->buffer.isObject()) {
        return rtThrowTypeError(std::string("Constructor ") + name + " requires 'new'");
    }
    Rooted<Value> arg{args[0]};

    bool ok = false;
    if (arg.get().isUndefined()) {
        ok = fillFromLength(receiver, kind, 0);
    } else if (arg.get().isNumber()) {
        uint32_t length = 0;
        if (!toIndex(arg.get(), "typed array", elementKindInfo(kind).bytesPerElement, length)) {
            return Value::fromUndefined();
        }
        ok = fillFromLength(receiver, kind, length);
    } else if (isBuffer(arg.get())) {
        ok = fillFromBuffer(receiver, kind, arg, args[1], args[2]);
    } else if (arg.get().isObject()) {
        const uint16_t flags = arg.get().asObject<HeapObjectHeader>()->flags;
        if (flags == TypedArrayHeader::kFlags) {
            ok = fillFromTypedArray(receiver, kind, arg);
        } else if (flags == HeapKind::Function) {
            return rtThrowTypeError(std::string(name) + " constructor: a function is not iterable");
        } else {
            ok = fillFromArrayLike(receiver, kind, arg);
        }
    } else {
        return rtThrowTypeError(std::string(name) +
                                " constructor requires a length, a buffer, an array or an iterable");
    }
    if (!ok) return Value::fromUndefined();
    return receiver.get();
}

// 23.2.4.2 TypedArrayCreateFromConstructor with a « length » argument list:
// `new C(length)` followed by 23.2.4.4 ValidateTypedArray and the length check
// of step 3.b.
Value rtTypedArrayCreateFromConstructor(Rooted<Value>& ctor, uint32_t length) {
    if (ElementKind kind; intrinsicViewKind(ctor.get(), kind)) {
        const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
        if (!checkAllocatable(length * bpe)) return Value::fromUndefined();
        return rtNewTypedArray(kind, length);
    }
    RootedBlock block(1);
    block.set(0, Value::fromDouble(static_cast<double>(length)));
    Rooted<Value> made{Value(bronze_construct(ctor.get().rawBits(), 1, block.data()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (!requireTypedArray(made.get(), "[[Construct]]")) return Value::fromUndefined();
    if (lengthOf(made.get()) < length) {
        return rtThrowTypeError("the constructed typed array is too short (" +
                                std::to_string(lengthOf(made.get())) + " < " +
                                std::to_string(length) + ")");
    }
    return made.get();
}

// 23.2.2.1 %TypedArray%.from(source [, mapFn [, thisArg]]). `this` is the
// constructor — an intrinsic, or a subclass reached through `extends` — and
// the result is 23.2.4.2's `new C(len)`, so `MyView.from([1, 2])` is a MyView.
uint64_t rtTypedArrayFromBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> ctor{Value(thisBits)};
    if (!rtIsConstructorValue(ctor.get())) {
        return rtThrowTypeError("%TypedArray%.from called on a value that is not a constructor")
            .rawBits();
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
        Rooted<Value> out{rtTypedArrayCreateFromConstructor(ctor, len)};
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
        Rooted<Value> out{rtTypedArrayCreateFromConstructor(ctor, len)};
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

    // Step 5: usingIterator is GetMethod(source, @@iterator), and when it is
    // undefined steps 9-13 read `length` and the indices instead —
    // `Uint8Array.from({ length: 2 }, fn)` is a two-element view, never an
    // "is not iterable" TypeError.
    if (!rtHasIteratorMethod(source)) {
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        const uint32_t len = rtArrayLikeLength(source);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Rooted<Value> out{rtTypedArrayCreateFromConstructor(ctor, len)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        for (uint32_t i = 0; i < len; ++i) {
            Rooted<Value> elem{rtArrayLikeElement(source, i)};
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
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
    return rtTypedArrayFromBody(0, ctor.get().rawBits(), 3, block);
}

// 23.2.2.2 %TypedArray%.of(...items).
uint64_t rtTypedArrayOfBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> ctor{Value(thisBits)};
    if (!rtIsConstructorValue(ctor.get())) {
        return rtThrowTypeError("%TypedArray%.of called on a value that is not a constructor")
            .rawBits();
    }
    Rooted<Value> out{rtTypedArrayCreateFromConstructor(ctor, args.count())};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    for (uint32_t i = 0; i < args.count(); ++i) {
        rtTypedArraySetElement(out, i, args[i]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return out.get().rawBits();
}

}  // namespace bronze::runtime
