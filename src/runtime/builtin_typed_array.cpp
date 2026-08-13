// The JS surface of `ArrayBuffer` and the nine views: the constructor objects,
// the four construction paths of 23.2.5.1, and the members an instance answers.
// The METHODS live next door in builtin_typed_array_methods.cpp; the
// representation lives in typed_array.{h,cpp}.
//
// The seam that shapes this file: a typed array constructor is an ordinary
// bronze function object, interned by code pointer, so `Float32Array` read as a
// bare name and `v.constructor` read off an instance are the SAME object and
// `===` between them holds. That is what three.js's `switch
// (array.constructor)` needs, and it is why the nine constructors are nine
// instantiations of one template rather than one function taking a kind — nine
// distinct code pointers is exactly what `bronze_function_singleton` interns
// on.

#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isBuffer(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == ArrayBufferHeader::kFlags;
}
bool isArray(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

// 7.1.22 ToIndex, and it is deliberately NOT "reject anything that is not an
// integer": ToIndex truncates, so `new Float32Array(1.5)` has one element and
// `new Float32Array(NaN)` has none. Only a negative value, an infinity, or
// something past 2^53-1 is the RangeError of step 2.c — which is a different
// error from "bronze cannot allocate that", and the two are kept apart so a
// program can tell a bad argument from a heap it has outgrown.
//
// Answering `false` leaves the error pending, which is what every caller here
// checks for.
bool toIndex(Value v, const char* what, uint32_t bytesPerElement, uint32_t& out) {
    if (v.isUndefined()) {
        out = 0;
        return true;
    }
    const double n = rtToNumber(v);
    // ToIntegerOrInfinity: NaN and both zeroes become +0, everything finite
    // truncates towards zero, and the infinities pass through to be rejected.
    double integer = std::isnan(n) ? 0.0 : std::trunc(n);
    if (integer == 0.0) integer = 0.0;  // normalise -0
    if (integer < 0.0 || integer > 9007199254740991.0) {
        rtThrowRangeError(std::string("Invalid ") + what + " length");
        return false;
    }
    if (integer > static_cast<double>(kMaxByteLength) / bytesPerElement) {
        rtThrowRangeError(std::string(what) + " allocation failed: length is too large");
        return false;
    }
    out = static_cast<uint32_t>(integer);
    return true;
}

// A buffer that a collection would have to COPY has to fit in a semispace,
// which is the concrete form "the buffer moves" takes. Diagnosed here, by name,
// rather than left to `std::bad_alloc` unwinding out of a helper that generated
// code called.
bool checkAllocatable(uint32_t byteLength) {
    const size_t semispace = rtHeap().reserved_size() / 2;
    if (byteLength >= kMaxByteLength || byteLength + 64 >= semispace) {
        rtThrowRangeError("Array buffer allocation failed: " + std::to_string(byteLength) +
                          " bytes does not fit in the heap");
        return false;
    }
    return true;
}

// ---- construction (23.2.5.1) ------------------------------------------------

// `new T(length)` and `new T()`.
Value fromLength(ElementKind kind, uint32_t length) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    if (!checkAllocatable(length * bpe)) return Value::fromUndefined();
    return Value::fromObject(TypedArrayHeader::create(rtHeap(), kind, length));
}

// `new T(buffer, byteOffset, length)` — 23.2.5.1 step 6, InitializeTypedArray-
// FromArrayBuffer. The view SHARES the buffer's bytes: two views over one
// buffer see each other's writes, which is the whole point of the form and
// what `extras/DataUtils.js` uses to reinterpret a float's bits.
Value fromBuffer(ElementKind kind, Rooted<Value>& buffer, Value offsetVal, Value lengthVal) {
    const uint32_t bpe = elementKindInfo(kind).bytesPerElement;
    uint32_t offset = 0;
    if (!toIndex(offsetVal, "byte offset", 1, offset)) return Value::fromUndefined();
    if (offset % bpe != 0) {
        rtThrowRangeError("start offset of " + std::string(elementKindInfo(kind).name) +
                          " should be a multiple of " + std::to_string(bpe));
        return Value::fromUndefined();
    }
    const uint32_t bufferLength = buffer.get().asObject<ArrayBufferHeader>()->byteLength;
    if (offset > bufferLength) {
        rtThrowRangeError("Start offset " + std::to_string(offset) +
                          " is outside the bounds of the buffer");
        return Value::fromUndefined();
    }

    uint32_t length = 0;
    if (lengthVal.isUndefined()) {
        // 23.2.5.1 step 6.d: an auto-length view spans the rest of the buffer,
        // and the remainder must divide evenly — a Float32Array over 6 bytes
        // is a RangeError, not a truncation.
        if ((bufferLength - offset) % bpe != 0) {
            rtThrowRangeError("byte length of " + std::string(elementKindInfo(kind).name) +
                              " should be a multiple of " + std::to_string(bpe));
            return Value::fromUndefined();
        }
        length = (bufferLength - offset) / bpe;
    } else {
        if (!toIndex(lengthVal, "typed array", bpe, length)) return Value::fromUndefined();
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(length) * bpe > bufferLength) {
            rtThrowRangeError("Invalid typed array length: " + std::to_string(length));
            return Value::fromUndefined();
        }
    }
    return Value::fromObject(
        TypedArrayHeader::createOverBuffer(rtHeap(), kind, buffer, offset, length));
}

// `new T(otherTypedArray)` — 23.2.5.1 step 5.b.i, InitializeTypedArrayFrom-
// TypedArray. A COPY with the destination's conversion applied per element,
// never a shared buffer: `new Uint8Array(f32)` is nine bytes of narrowing, not
// a reinterpretation of the float's bits (which is what `new Uint8Array(
// f32.buffer)` would be).
Value fromTypedArray(ElementKind kind, Rooted<Value>& source) {
    const uint32_t length = source.get().asObject<TypedArrayHeader>()->length;
    Rooted<Value> out{fromLength(kind, length)};
    if (rtExceptionPending()) return Value::fromUndefined();
    for (uint32_t i = 0; i < length; ++i) {
        // Both pointers are re-derived every step. Nothing in the loop
        // allocates today, but the rule that keeps this correct is that a raw
        // view pointer never outlives a statement.
        const double v = source.get().asObject<TypedArrayHeader>()->get(i);
        out.get().asObject<TypedArrayHeader>()->set(i, v);
    }
    return out.get();
}

// `new T(arrayLike)` and `new T(iterable)` — 23.2.5.1 step 5.b.ii. An array
// is walked directly rather than through its iterator: the answer is the same
// and the iterator form allocates a record and a cursor per element.
Value fromArrayLike(ElementKind kind, Rooted<Value>& source) {
    if (isArray(source.get())) {
        const uint32_t length = source.get().asObject<ArrayHeader>()->length;
        Rooted<Value> out{fromLength(kind, length)};
        if (rtExceptionPending()) return Value::fromUndefined();
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
            const double v = rtToNumber(elem.get());
            out.get().asObject<TypedArrayHeader>()->set(i, v);
        }
        return out.get();
    }

    // Anything else iterable: collect first, because the element count is not
    // known until the walk ends and a typed array cannot grow.
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

// Nine instantiations, so nine distinct code pointers, so nine distinct
// interned function objects — which is what makes `Int8Array !== Uint8Array`
// and `x.constructor === Float32Array` both true.
template <ElementKind K>
uint64_t typedArrayCtor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return constructTypedArray(K, argc, argv).rawBits();
}

uint64_t arrayBufferCtor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    uint32_t byteLength = 0;
    if (!toIndex(args[0], "array buffer", 1, byteLength)) return Value::fromUndefined().rawBits();
    if (!checkAllocatable(byteLength)) return Value::fromUndefined().rawBits();
    // A byteLength of 0 is legal and produces a real buffer: `byteLength` is
    // 0, every view over it has length 0, and it is still an eight-byte heap
    // object, which is what keeps the inline property fast path's
    // unconditional header load safe (BRONZE_ABI_OBJ_MIN_PAYLOAD).
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
};

static_assert(std::size(kCtors) == static_cast<size_t>(ElementKind::Count),
              "the constructor table has drifted from the ElementKind enum");

// Real members of `%TypedArray%.prototype` that bronze has not built. A name
// leaves this list when it lands (rt_members.cpp's rule); what is here is the
// ECMA-262 question "does this exist?", so reading one is a named error and
// never `undefined`.
const char* const kTypedArrayUnimplemented[] = {
    "at",         "entries",     "every",    "filter",  "find",        "findIndex",
    "findLast",   "findLastIndex", "keys",   "lastIndexOf", "reduce",  "reduceRight",
    "reverse",    "some",        "sort",     "toLocaleString", "toReversed", "toSorted",
    "toString",   "values",      "with",
};

// Real members of `ArrayBuffer.prototype`, minus `byteLength`, which is real.
const char* const kArrayBufferUnimplemented[] = {
    "detached", "maxByteLength", "resizable", "resize", "slice", "transfer",
    "transferToFixedLength",
};

// The five slot accessors and the 10.2.5 back-pointer, by name. They are a
// ladder in `rtTypedArrayMember`, which needs each one's field, and a list here
// for `rtTypedArrayHasMember`, which needs only the name — one list, so the two
// readers cannot come to disagree about what a typed array HAS.
const char* const kTypedArraySlotMembers[] = {
    "length", "byteLength", "byteOffset", "buffer", "BYTES_PER_ELEMENT", "constructor",
};

}  // namespace

Value rtTypedArrayConstructor(const std::string& name) {
    if (name == "ArrayBuffer") return Value(bronze_function_singleton(arrayBufferCtor, 0));
    for (const CtorEntry& entry : kCtors) {
        if (name == elementKindInfo(entry.kind).name) {
            // Arity 0: a variadic native must not be padded, or
            // `new Float32Array(buf)` would arrive with two extra undefined
 // arguments and take the three-argument branch.
            return Value(bronze_function_singleton(entry.code, 0));
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
        if (entry.kind == kind) return Value(bronze_function_singleton(entry.code, 0));
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
        // 23.2.6.2: the only own data property a %TypedArray% constructor
        // carries. Answering `undefined` for it would be a silent lie about a
        // property ECMA-262 defines, which is exactly what rt_members.cpp
        // exists to prevent — and it is two lines, so it is built rather than
        // diagnosed.
        if (key == "BYTES_PER_ELEMENT") {
            out = Value::fromDouble(elementKindInfo(entry.kind).bytesPerElement);
            return true;
        }
        return false;
    }
    return false;
}

Value rtTypedArrayMember(Value viewVal, const std::string& key) {
    auto* view = viewVal.asObject<TypedArrayHeader>();
    if (key == "length") return Value::fromDouble(view->length);
    if (key == "byteLength") return Value::fromDouble(view->byteLength());
    if (key == "byteOffset") return Value::fromDouble(view->byteOffset);
    if (key == "buffer") return view->buffer;
    if (key == "BYTES_PER_ELEMENT") return Value::fromDouble(view->bytesPerElement());
    // The 10.2.5 back-pointer, as a real one: the same object the bare name
    // resolves to, so `x.constructor === Float32Array` and the `switch` over
    // constructors in three.js's MathUtils both work.
    if (key == "constructor") return rtTypedArrayConstructorFor(view->elementKind());

    // Everything below can allocate a function object, so `view` — and
    // `viewVal`, which is a by-value copy of bits the collector will not
    // update — must not be read again. The kind's name is an immortal string
    // literal in the element table, so reading it HERE and holding it across
    // the allocation is the one thing that is safe to carry.
    const char* kindName = view->kindName();
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
    if (key == "byteLength") {
        return Value::fromDouble(bufferVal.asObject<ArrayBufferHeader>()->byteLength);
    }
    if (key == "constructor") return rtTypedArrayConstructor("ArrayBuffer");
    rtCheckUnimplementedMember("ArrayBuffer.prototype", kArrayBufferUnimplemented,
                               std::size(kArrayBufferUnimplemented), key);
    return Value::fromUndefined();
}

// The two receivers above asked whether a member EXISTS, which is `in`'s
// question and not a read's. Both walk the same lists their readers walk and
// end at the same named refusal, so `'sort' in v` is the diagnostic `v.sort`
// is, rather than a `false` that contradicts it. Nothing here allocates: the
// caller holds a header across the call.
//
// The INDEX half of a typed array is not here — 10.4.5 makes an integer index a
// different question from a member name, and the caller answers it against the
// view's length before it ever gets this far.
bool rtTypedArrayHasMember(const char* kindName, const std::string& key) {
    for (const char* name : kTypedArraySlotMembers) {
        if (key == name) return true;
    }
    if (rtTypedArrayHasMethod(key)) return true;
    rtCheckTypedArrayMember(kindName, key);
    return false;
}

bool rtArrayBufferHasMember(const std::string& key) {
    if (key == "byteLength" || key == "constructor") return true;
    rtCheckUnimplementedMember("ArrayBuffer.prototype", kArrayBufferUnimplemented,
                               std::size(kArrayBufferUnimplemented), key);
    return false;
}

}  // namespace bronze::runtime
