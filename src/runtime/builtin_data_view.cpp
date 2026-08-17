// The JS surface of `DataView` (ECMA-262 25.3): the constructor and its
// validation ladder, the three slot accessors, and the sixteen accessors that
// read and write one number at a byte offset. The representation — the header
// and its GC rule — is in typed_array.{h,cpp}, beside the ArrayBuffer it
// windows.
//
// The seam that shapes this file, and the reason DataView is not a tenth
// ElementKind: a %TypedArray% access is `base + index * width`, with the width
// and the byte order both fixed by the view. Here the width is chosen by the
// METHOD and the byte order by an argument, so neither is a property of the
// object — `getFloat32(1, true)` is unaligned by design and little-endian by
// request, and nothing in 25.3 has an alignment requirement at all.
//
// The other rule this whole file turns on: `isLittleEndian` DEFAULTS TO FALSE.
// DataView is the one place in the language whose byte order is not the
// platform's, so the bytes are assembled by explicit shifting below and never
// by copying a native integer — a `memcpy` of a `uint16_t` would be right on
// this machine, wrong on the other kind, and identical to read either way.

#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isBuffer(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == ArrayBufferHeader::kFlags;
}

bool isDataView(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == DataViewHeader::kFlags;
}

// ---- the byte order (25.3.1.3 GetValueFromBuffer / 25.3.1.4 SetValueInBuffer)
//
// The two functions the host's endianness must not reach. They index the bytes
// of the buffer explicitly and move them through an integer register one shift
// at a time, so the ONLY thing that decides which end goes first is the
// `littleEndian` argument. There is no `uint16_t*` and no `memcpy` of a native
// integer anywhere in this file, because both of those spell "whatever this
// machine does" and 25.3's default is big-endian regardless of the machine.

uint64_t readRawBytes(const uint8_t* p, uint32_t count, bool littleEndian) noexcept {
    uint64_t bits = 0;
    for (uint32_t i = 0; i < count; ++i) {
        // Most significant byte first. Big-endian takes it from the front of
        // the window, little-endian from the back; the shift below is the same
        // either way, which is what keeps the two orders one function.
        const uint32_t at = littleEndian ? count - 1 - i : i;
        bits = (bits << 8) | static_cast<uint64_t>(p[at]);
    }
    return bits;
}

void writeRawBytes(uint8_t* p, uint32_t count, bool littleEndian, uint64_t bits) noexcept {
    for (uint32_t i = 0; i < count; ++i) {
        // Least significant byte first, into the end of the window big-endian
        // puts it at.
        const uint32_t at = littleEndian ? i : count - 1 - i;
        p[at] = static_cast<uint8_t>(bits & 0xFFu);
        bits >>= 8;
    }
}

// ---- 25.3.1.5 NumericToRawBytes / 25.3.1.6 RawBytesToNumeric -----------------
//
// The value <-> bit-pattern half, with no byte order in it at all: these two
// speak in an integer whose bit N means bit N of the IEEE-754 or two's
// -complement encoding, and the functions above are the only place that decides
// which byte of memory holds which eight of those bits.
//
// `bit_cast` is what carries a float to its encoding. It is not a byte-order
// decision: it names the object representation of the two types as one, and
// every platform that has IEEE-754 floats at all agrees on which bit is the
// sign. The byte order of that representation never escapes, because the value
// leaves here as an integer.

// The canonical quiet NaN of each width. 25.3.1.5 step 1 allows any NaN
// encoding ("an implementation-defined choice"), which is exactly the freedom
// the deterministic-output rule forbids bronze from taking twice: `(float)NaN`
// is a compiler's choice of payload, so the pattern is written here instead.
constexpr uint32_t kQuietNaN32 = 0x7FC00000u;
constexpr uint64_t kQuietNaN64 = 0x7FF8000000000000ull;

uint64_t numericToRawBits(ElementKind kind, double value) noexcept {
    // binary16 is typed_array.cpp's conversion — computed from the double,
    // round-half-to-even, NaN already canonical — so the two stores agree.
    if (kind == ElementKind::Float16) return doubleToFloat16Bits(value);
    if (kind == ElementKind::Float32) {
        const float narrowed = static_cast<float>(value);
        if (std::isnan(narrowed)) return kQuietNaN32;
        return std::bit_cast<uint32_t>(narrowed);
    }
    if (kind == ElementKind::Float64) {
        if (std::isnan(value)) return kQuietNaN64;
        return std::bit_cast<uint64_t>(value);
    }
    // Table 70's conversion operation for the integer types is ToInt8 ..
    // ToUint32, which is what a store to the matching typed array performs —
    // so it is the same function, and the two cannot disagree about what 1e40
    // narrows to.
    double integer = convertForStore(kind, value);
    const uint32_t bits = elementKindInfo(kind).bytesPerElement * 8;
    // A signed result arrives in [-2^(n-1), 2^(n-1)-1]; its two's-complement
    // pattern is the same number modulo 2^n.
    if (integer < 0.0) integer += std::ldexp(1.0, static_cast<int>(bits));
    return static_cast<uint64_t>(integer);
}

double rawBitsToNumeric(ElementKind kind, uint64_t bits) noexcept {
    switch (kind) {
        case ElementKind::Int8: return static_cast<int8_t>(static_cast<uint8_t>(bits));
        case ElementKind::Uint8: return static_cast<uint8_t>(bits);
        case ElementKind::Int16: return static_cast<int16_t>(static_cast<uint16_t>(bits));
        case ElementKind::Uint16: return static_cast<uint16_t>(bits);
        case ElementKind::Int32: return static_cast<int32_t>(static_cast<uint32_t>(bits));
        case ElementKind::Uint32: return static_cast<uint32_t>(bits);
        case ElementKind::Float16: return float16BitsToDouble(static_cast<uint16_t>(bits));
        case ElementKind::Float32:
            return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(bits)));
        case ElementKind::Float64: return std::bit_cast<double>(bits);
        case ElementKind::Uint8Clamped:
        case ElementKind::Count: break;
    }
    // Table 70 has no clamped entry: clamping is a rule about STORING to a
    // Uint8ClampedArray element, and no DataView accessor names it.
    fatal("internal: a DataView access with an element type outside table 70");
}

// ---- the validation ladder --------------------------------------------------

// 7.1.22 ToIndex, in the one spelling every clause of 25.3 that takes a byte
// position uses. It truncates rather than rejecting a fraction — `getUint8(1.9)`
// reads byte 1 — and it is a RangeError, never a TypeError, for a negative
// value or one past 2^53-1.
//
// `false` leaves the error pending, which every caller here checks for.
bool toIndex(Value v, const char* what, uint32_t& out) {
    if (v.isUndefined()) {
        out = 0;
        return true;
    }
    const double n = rtToNumber(v);
    if (rtExceptionPending()) return false;
    // ToIntegerOrInfinity: NaN and both zeroes become +0, everything finite
    // truncates towards zero, and the infinities pass through to be rejected.
    double integer = std::isnan(n) ? 0.0 : std::trunc(n);
    if (integer == 0.0) integer = 0.0;  // normalise -0
    if (integer < 0.0 || integer > 9007199254740991.0) {
        rtThrowRangeError(what);
        return false;
    }
    // Every buffer bronze will allocate is under kMaxByteLength (typed_array.h
    // says why the cap is not a comfort), so an index above it cannot be in
    // range for any view and the narrowing below would wrap. Reported as the
    // out-of-range it is rather than as an overflow.
    if (integer > static_cast<double>(kMaxByteLength)) {
        rtThrowRangeError(what);
        return false;
    }
    out = static_cast<uint32_t>(integer);
    return true;
}

// The message both index failures share. 25.3.1.1 step 3 (ToIndex) and step 9
// (the bounds test) are two RangeErrors about the same thing — a byte position
// this view does not have — and a program that catches one catches the other.
constexpr const char* kOutOfBounds = "Offset is outside the bounds of the DataView";

// 25.3.1.1 GetViewValue steps 1..2 / 25.3.1.2 SetViewValue steps 1..2:
// RequireInternalSlot([[DataView]]). A TypeError, so that a detached
// `const g = view.getUint16; g(0)` names what it is rather than reading whatever
// `this` happened to be.
bool requireDataView(Value v, const char* method) {
    if (isDataView(v)) return true;
    rtThrowTypeError(std::string("DataView.prototype.") + method +
                     " called on a value that is not a DataView");
    return false;
}

// 25.3.1.1 GetViewValue.
Value getViewValue(Rooted<Value>& self, Value requestIndex, Value littleEndianArg,
                   ElementKind kind) {
    // Step 1's ToIndex is 7.1.4 under a different name, so `getInt32(obj)` runs
    // a user `valueOf` — which allocates and moves the OTHER argument. The
    // caller's `RootedArgs` roots its own slots, but the copies handed here are
    // ordinary locals the collector cannot see, so they get roots of their own.
    Rooted<Value> endianRoot{littleEndianArg};
    uint32_t getIndex = 0;
    if (!toIndex(requestIndex, kOutOfBounds, getIndex)) return Value::fromUndefined();
    // Step 4 is ToBoolean, not a strict test: `getUint16(0, 1)` is
    // little-endian, and an omitted argument is `undefined`, which is false —
    // which is the whole of "defaults to big-endian".
    const bool littleEndian = bronze_truthy(endianRoot.get().rawBits());
    const uint32_t elementSize = elementKindInfo(kind).bytesPerElement;

    // Derived after the last thing that could allocate, and used before the
    // next: `bytes()` names memory the collector moves.
    auto* view = self.get().asObject<DataViewHeader>();
    if (static_cast<uint64_t>(getIndex) + elementSize > view->byteLength) {
        return rtThrowRangeError(kOutOfBounds);
    }
    const uint64_t bits = readRawBytes(view->bytes() + getIndex, elementSize, littleEndian);
    return Value::fromDouble(rawBitsToNumeric(kind, bits));
}

// 25.3.1.2 SetViewValue.
Value setViewValue(Rooted<Value>& self, Value requestIndex, Value littleEndianArg, ElementKind kind,
                   Value value) {
    // Rooted for the reason getViewValue's are, and with one more operand at
    // risk: `setInt32(objA, objB)` runs two conversions, and the first can move
    // what the second is about to read.
    Rooted<Value> endianRoot{littleEndianArg};
    Rooted<Value> valueRoot{value};
    uint32_t getIndex = 0;
    if (!toIndex(requestIndex, kOutOfBounds, getIndex)) return Value::fromUndefined();
    // Step 4 runs ToNumber BEFORE the bounds test of step 11, so a `set` past
    // the end of the view still converts its value first — and a conversion
    // that throws is what such a call reports, not the RangeError.
    const double num = rtToNumber(valueRoot.get());
    if (rtExceptionPending()) return Value::fromUndefined();
    const bool littleEndian = bronze_truthy(endianRoot.get().rawBits());
    const uint32_t elementSize = elementKindInfo(kind).bytesPerElement;

    auto* view = self.get().asObject<DataViewHeader>();
    if (static_cast<uint64_t>(getIndex) + elementSize > view->byteLength) {
        return rtThrowRangeError(kOutOfBounds);
    }
    writeRawBytes(view->bytes() + getIndex, elementSize, littleEndian,
                  numericToRawBits(kind, num));
    return Value::fromUndefined();
}

// ---- the four 64-bit accessors (25.3.4.5, .6, .19, .20) ---------------------
//
// They are not `dvGet<K>`/`dvSet<K>` instantiations because their VALUE is a
// BigInt: `ElementKind` has no 64-bit integer member, and the number half of
// this file speaks in doubles, which cannot carry 2^63 - 1. What they share
// with the sixteen above is everything else — the same ToIndex, the same
// bounds test, the same explicit byte order.

Value getBigViewValue(Rooted<Value>& self, Value requestIndex, Value littleEndianArg,
                      bool isSigned) {
    Rooted<Value> endianRoot{littleEndianArg};
    uint32_t getIndex = 0;
    if (!toIndex(requestIndex, kOutOfBounds, getIndex)) return Value::fromUndefined();
    const bool littleEndian = bronze_truthy(endianRoot.get().rawBits());
    auto* view = self.get().asObject<DataViewHeader>();
    if (static_cast<uint64_t>(getIndex) + 8 > view->byteLength) {
        return rtThrowRangeError(kOutOfBounds);
    }
    const uint64_t bits = readRawBytes(view->bytes() + getIndex, 8, littleEndian);
    // The read is finished before this allocates, which is what lets the raw
    // `view` pointer above be used and then dropped. The conversion is
    // bigint.h's, shared with the BigInt64Array element path so that the two
    // cannot disagree about a sign bit.
    return rtBigIntFromRawBits64(bits, isSigned);
}

Value setBigViewValue(Rooted<Value>& self, Value requestIndex, Value littleEndianArg,
                      Value value, bool isSigned) {
    Rooted<Value> endianRoot{littleEndianArg};
    Rooted<Value> valueRoot{value};
    uint32_t getIndex = 0;
    if (!toIndex(requestIndex, kOutOfBounds, getIndex)) return Value::fromUndefined();
    // Step 4 is ToBigInt and it runs BEFORE the bounds test, exactly as the
    // number accessors' ToNumber does — so a `set` past the end of the view
    // still refuses a Number argument first. The conversion and the modulo-2^64
    // wrap are bigint.h's, shared with the BigInt64Array element path.
    uint64_t bits = 0;
    if (!rtBigIntToRawBits64(valueRoot.get(), bits)) return Value::fromUndefined();
    (void)isSigned;  // the stored bits are the same 64 either way

    const bool littleEndian = bronze_truthy(endianRoot.get().rawBits());
    auto* view = self.get().asObject<DataViewHeader>();
    if (static_cast<uint64_t>(getIndex) + 8 > view->byteLength) {
        return rtThrowRangeError(kOutOfBounds);
    }
    writeRawBytes(view->bytes() + getIndex, 8, littleEndian, bits);
    return Value::fromUndefined();
}

template <bool Signed>
uint64_t dvGetBig(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireDataView(self.get(), Signed ? "getBigInt64" : "getBigUint64")) {
        return Value::fromUndefined().rawBits();
    }
    return getBigViewValue(self, args[0], args[1], Signed).rawBits();
}

template <bool Signed>
uint64_t dvSetBig(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireDataView(self.get(), Signed ? "setBigInt64" : "setBigUint64")) {
        return Value::fromUndefined().rawBits();
    }
    return setBigViewValue(self, args[0], args[2], args[1], Signed).rawBits();
}

// ---- the accessors ----------------------------------------------------------
//
// The name each accessor answers to, derived from the element type rather than
// passed in, so a method cannot come to be named after a different width than
// the one it reads. Declared here and defined below because the templates that
// use it are the reason it exists.
const char* accessorName(ElementKind kind, bool isGet) noexcept;

//
// Sixteen instantiations, so sixteen distinct code pointers, so sixteen
// distinct interned function objects — `bronze_function_singleton` interns on
// the code pointer, so a single function taking the element type as data would
// make `view.getUint8 === view.getFloat64` true.

template <ElementKind K>
uint64_t dvGet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireDataView(self.get(), accessorName(K, /*isGet=*/true))) {
        return Value::fromUndefined().rawBits();
    }
    return getViewValue(self, args[0], args[1], K).rawBits();
}

template <ElementKind K>
uint64_t dvSet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireDataView(self.get(), accessorName(K, /*isGet=*/false))) {
        return Value::fromUndefined().rawBits();
    }
    return setViewValue(self, args[0], args[2], K, args[1]).rawBits();
}

// ---- the constructor (25.3.2.1) ---------------------------------------------

uint64_t dataViewCtor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 2, RequireInternalSlot(buffer, [[ArrayBufferData]]): a TypeError,
    // and the one rung of this ladder that is not a RangeError. The difference
    // is the spec's and it is the ordinary one — a wrong KIND of argument is a
    // TypeError, a wrong VALUE of the right kind is a RangeError.
    if (!isBuffer(args[0])) {
        return rtThrowTypeError("First argument to DataView constructor must be an ArrayBuffer")
            .rawBits();
    }
    Rooted<Value> buffer{args[0]};

    // Step 3, ToIndex(byteOffset): RangeError for a negative offset or one past
    // 2^53-1, and a truncation for a fraction.
    uint32_t offset = 0;
    if (!toIndex(args[1], "Invalid DataView byte offset", offset)) {
        return Value::fromUndefined().rawBits();
    }
    const uint32_t bufferLength = buffer.get().asObject<ArrayBufferHeader>()->byteLength;
    // Step 6: an offset the buffer does not reach. RangeError, and separate
    // from the step above because `-1` and `9` fail for different reasons.
    if (offset > bufferLength) {
        return rtThrowRangeError("Start offset " + std::to_string(offset) +
                                 " is outside the bounds of the buffer")
            .rawBits();
    }

    uint32_t byteLength = 0;
    if (args[2].isUndefined()) {
        // Step 8: an omitted length spans the rest of the buffer. There is no
        // divisibility condition to fail here, which is the plainest sign that
        // this object has no element width — a Float32Array over the same six
        // remaining bytes is a RangeError.
        byteLength = bufferLength - offset;
    } else {
        // Step 9: ToIndex again, then the window must fit.
        if (!toIndex(args[2], "Invalid DataView length", byteLength)) {
            return Value::fromUndefined().rawBits();
        }
        if (static_cast<uint64_t>(offset) + byteLength > bufferLength) {
            return rtThrowRangeError("Invalid DataView length").rawBits();
        }
    }
    return Value::fromObject(DataViewHeader::create(rtHeap(), buffer, offset, byteLength))
        .rawBits();
}

// ---- the member table -------------------------------------------------------

struct Accessor {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;  // the `length` 25.3.4 gives the method
};

const Accessor kAccessors[] = {
    {"getInt8", dvGet<ElementKind::Int8>, 1},
    {"getUint8", dvGet<ElementKind::Uint8>, 1},
    {"getInt16", dvGet<ElementKind::Int16>, 1},
    {"getUint16", dvGet<ElementKind::Uint16>, 1},
    {"getInt32", dvGet<ElementKind::Int32>, 1},
    {"getUint32", dvGet<ElementKind::Uint32>, 1},
    {"getFloat32", dvGet<ElementKind::Float32>, 1},
    {"getFloat64", dvGet<ElementKind::Float64>, 1},
    {"setInt8", dvSet<ElementKind::Int8>, 2},
    {"setUint8", dvSet<ElementKind::Uint8>, 2},
    {"setInt16", dvSet<ElementKind::Int16>, 2},
    {"setUint16", dvSet<ElementKind::Uint16>, 2},
    {"setInt32", dvSet<ElementKind::Int32>, 2},
    {"setUint32", dvSet<ElementKind::Uint32>, 2},
    {"setFloat32", dvSet<ElementKind::Float32>, 2},
    {"setFloat64", dvSet<ElementKind::Float64>, 2},
    {"getBigInt64", dvGetBig<true>, 1},
    {"getBigUint64", dvGetBig<false>, 1},
    {"setBigInt64", dvSetBig<true>, 2},
    {"setBigUint64", dvSetBig<false>, 2},
    {"getFloat16", dvGet<ElementKind::Float16>, 1},
    {"setFloat16", dvSet<ElementKind::Float16>, 2},
};

const char* accessorName(ElementKind kind, bool isGet) noexcept {
    switch (kind) {
        case ElementKind::Int8: return isGet ? "getInt8" : "setInt8";
        case ElementKind::Uint8: return isGet ? "getUint8" : "setUint8";
        case ElementKind::Int16: return isGet ? "getInt16" : "setInt16";
        case ElementKind::Uint16: return isGet ? "getUint16" : "setUint16";
        case ElementKind::Int32: return isGet ? "getInt32" : "setInt32";
        case ElementKind::Uint32: return isGet ? "getUint32" : "setUint32";
        case ElementKind::Float16: return isGet ? "getFloat16" : "setFloat16";
        case ElementKind::Float32: return isGet ? "getFloat32" : "setFloat32";
        case ElementKind::Float64: return isGet ? "getFloat64" : "setFloat64";
        case ElementKind::Uint8Clamped:
        case ElementKind::Count: break;
    }
    fatal("internal: a DataView accessor named for an element type outside table 70");
}

}  // namespace

Value rtDataViewConstructor(const std::string& name) {
    if (name != "DataView") return Value::fromUndefined();
    // Arity 0 for the reason the nine view constructors take it: a variadic
    // native must not be padded, or `new DataView(buffer)` would arrive with
    // two extra `undefined`s and take the explicit-length branch. Interned by
    // code pointer, so the bare name and `v.constructor` are the SAME object.
    return rtNativeFunction(dataViewCtor, 0);
}

const char* rtDataViewConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    return fn.asObject<FunctionHeader>()->code == dataViewCtor ? "DataView" : nullptr;
}

Value rtDataViewMember(Value viewVal, const std::string& key) {
    auto* view = viewVal.asObject<DataViewHeader>();
    // 25.3.4.1..25.3.4.3, which are the view's own slots and not the buffer's:
    // a windowed view reports the window.
    if (key == "byteLength") return Value::fromDouble(view->byteLength);
    if (key == "byteOffset") return Value::fromDouble(view->byteOffset);
    if (key == "buffer") return view->buffer;
    // Everything below can allocate a function object, so `view` must not be
    // read again.
    if (key == "constructor") return rtDataViewConstructor("DataView");
    for (const Accessor& a : kAccessors) {
        if (key == a.name) return rtNativeFunction(a.code, a.arity);
    }
    return Value::fromUndefined();
}

bool rtDataViewHasMember(const std::string& key) {
    if (key == "byteLength" || key == "byteOffset" || key == "buffer" || key == "constructor") {
        return true;
    }
    for (const Accessor& a : kAccessors) {
        if (key == a.name) return true;
    }
    return false;
}

}  // namespace bronze::runtime
