// The shared-memory surface: `SharedArrayBuffer` (ECMA-262 25.2) and the
// `Atomics` namespace (25.4).
//
// The seam is SHARED MEMORY, not "more buffer methods", and the two halves are
// here together because neither is meaningful without the other's premise:
// bronze runs exactly ONE agent. There is no worker, no second thread and no
// `postMessage`, so a SharedArrayBuffer's bytes are shared with nobody and
// every atomic operation below is an ordinary load or store that cannot be
// observed half-done. That is not a shortcut — with one agent it is the whole
// of the memory model (25.4.1's ordering constraints are vacuous when the
// candidate execution has a single agent's events in it), and it is why
// `Atomics.wait`, `waitAsync` and `notify` are refused by name rather than
// implemented: they exist to coordinate with an agent bronze cannot create.
//
// What is NOT here: the representation, which is `ArrayBufferHeader` with
// `kFlagShared` set (typed_array.h explains why a flag and not a kind), and the
// members of a NON-shared buffer, which stay in builtin_typed_array.cpp. This
// file owns the divergence between the two surfaces and nothing else.

#include <cmath>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

thread_local Value g_atomicsObject = Value::fromUndefined();

bool isSharedBuffer(Value v) {
    return isBuffer(v) && v.asObject<ArrayBufferHeader>()->isShared();
}

// ---- 25.2 SharedArrayBuffer ------------------------------------------------

// The intrinsic: `SharedArrayBuffer`, its prototype, and the instance shape
// every construction allocates from. Permanent roots; built on first use.
struct SharedBufferIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Shape* instanceShape = nullptr;
};

thread_local SharedBufferIntrinsics g_shared;

void ensureSharedArrayBufferIntrinsics();

// 25.2.3.1. `{maxByteLength}` makes it GROWABLE, which is the shared spelling
// of resizable; a plain `new SharedArrayBuffer(n)` is fixed.
//
// The receiver is a zero-byte PLACEHOLDER carrying NewTarget's prototype, for
// the reason builtin_array_buffer.cpp gives: the byte count is an argument
// converted after the allocation site has run. The real buffer is built here
// with the placeholder's shape and RETURNED, which is what `new` answers.
uint64_t sharedArrayBufferCtor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    if (!rtIsNativeConstructReceiver(receiver.get()) || !isSharedBuffer(receiver.get())) {
        return rtThrowTypeError("Constructor SharedArrayBuffer requires 'new'").rawBits();
    }
    uint32_t byteLength = 0;
    if (!toIndex(args[0], "shared array buffer", 1, byteLength)) {
        return Value::fromUndefined().rawBits();
    }
    if (!checkAllocatable(byteLength)) return Value::fromUndefined().rawBits();

    uint32_t maxByteLength = byteLength;
    if (args.count() > 1 && args[1].isObject()) {
        Rooted<Value> opts{args[1]};
        Rooted<Value> mblKey{rtMakeString("maxByteLength")};
        Rooted<Value> mblVal{
            Value(bronze_elem_get(opts.get().rawBits(), mblKey.get().rawBits()))};
        if (!mblVal.get().isUndefined()) {
            if (!toIndex(mblVal.get(), "maxByteLength", 1, maxByteLength)) {
                return Value::fromUndefined().rawBits();
            }
            if (maxByteLength < byteLength) {
                return rtThrowRangeError("maxByteLength must be >= byteLength").rawBits();
            }
            if (!checkAllocatable(maxByteLength)) return Value::fromUndefined().rawBits();
        }
    }
    // Read AFTER every conversion above: each can allocate and move the
    // placeholder, and the shape is what carries NewTarget's answer.
    Shape* shape = receiver.get().asObject<ArrayBufferHeader>()->object.shape;
    return Value::fromObject(
               ArrayBufferHeader::createShared(rtHeap(), shape, byteLength, maxByteLength))
        .rawBits();
}

// 25.2.5's getters open with RequireInternalSlot and then REQUIRE the shared
// brand (step 3 of each): an ArrayBuffer is a TypeError here, as a
// SharedArrayBuffer is on the other prototype.
bool requireSharedBuffer(Value v, const char* member) {
    if (isSharedBuffer(v)) return true;
    rtThrowTypeError(std::string("SharedArrayBuffer.prototype.") + member +
                     " called on incompatible receiver");
    return false;
}

// 25.2.5.1 get byteLength.
uint64_t sharedByteLengthGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireSharedBuffer(self, "byteLength")) return Value::fromUndefined().rawBits();
    return Value::fromDouble(self.asObject<ArrayBufferHeader>()->byteLength).rawBits();
}

// 25.2.5.5 get maxByteLength.
uint64_t sharedMaxByteLengthGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireSharedBuffer(self, "maxByteLength")) return Value::fromUndefined().rawBits();
    return Value::fromDouble(self.asObject<ArrayBufferHeader>()->maxByteLength).rawBits();
}

// 25.2.5.3 get growable.
uint64_t sharedGrowableGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireSharedBuffer(self, "growable")) return Value::fromUndefined().rawBits();
    return Value::fromBool(self.asObject<ArrayBufferHeader>()->isResizable()).rawBits();
}

// 25.2.5.4 SharedArrayBuffer.prototype.grow. `resize` with one difference that
// is the whole point of the separate name: shared memory can never SHRINK,
// because another agent's view of the bytes it dropped would be unanswerable.
// bronze has no other agent, and refuses anyway — the program that shrinks a
// SAB is wrong about the memory model wherever it runs.
uint64_t sharedArrayBufferGrow(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!isSharedBuffer(self)) {
        return rtThrowTypeError(
                   "SharedArrayBuffer.prototype.grow called on non-SharedArrayBuffer")
            .rawBits();
    }
    if (!self.asObject<ArrayBufferHeader>()->isResizable()) {
        return rtThrowTypeError("Cannot grow a non-growable SharedArrayBuffer").rawBits();
    }
    uint32_t newLen = 0;
    // ToIndex can run user code, and the buffer is a heap pointer — so the
    // header is re-derived after the conversion and not before it.
    Rooted<Value> selfRoot{self};
    if (!toIndex(args[0], "byte length", 1, newLen)) return Value::fromUndefined().rawBits();
    auto* buf = selfRoot.get().asObject<ArrayBufferHeader>();
    if (newLen > buf->maxByteLength) {
        return rtThrowRangeError("Invalid byte length: exceeds maxByteLength").rawBits();
    }
    if (newLen < buf->byteLength) {
        return rtThrowRangeError("SharedArrayBuffer cannot shrink").rawBits();
    }
    std::memset(buf->data() + buf->byteLength, 0, newLen - buf->byteLength);
    buf->byteLength = newLen;
    // A grow can never strand a fixed view, but a length-TRACKING one
    // (`new Uint8Array(sab)` over a growable buffer) recomputes its window
    // from exactly this mutation — same walk, same reason as `resize`.
    closeOrReopenViews(rtHeap(), selfRoot);
    return Value::fromUndefined().rawBits();
}

// 25.2.5.6 steps 12-19: SpeciesConstructor(O, %SharedArrayBuffer%) and
// `new ctor(len)`, with the checks on what came back. A buffer of the
// intrinsic's own shape — no own `constructor`, the intrinsic prototype —
// skips the two `Get`s: a subclass instance or a decorated one takes them.
Value sharedSpeciesNew(Rooted<Value>& self, uint32_t newLen) {
    if (self.get().asObject<ArrayBufferHeader>()->object.shape == g_shared.instanceShape) {
        return rtNewSharedArrayBuffer(newLen, newLen);
    }
    Rooted<Value> ctorKey{rtMakeString("constructor")};
    Rooted<Value> ctor{Value(bronze_elem_get(self.get().rawBits(), ctorKey.get().rawBits()))};
    if (ctor.get().isUndefined()) ctor.set(g_shared.ctor);
    if (!ctor.get().isObject()) {
        return rtThrowTypeError("the constructor of this SharedArrayBuffer is not an object");
    }
    Rooted<Value> speciesKey{Value::fromSymbol(rtSymbolSpecies())};
    Rooted<Value> species{
        Value(bronze_elem_get(ctor.get().rawBits(), speciesKey.get().rawBits()))};
    if (species.get().isUndefined() || species.get().isNull()) species.set(g_shared.ctor);
    if (!rtIsConstructorValue(species.get())) {
        return rtThrowTypeError("[Symbol.species] of this SharedArrayBuffer is not a constructor");
    }
    RootedBlock block(1);
    block.set(0, Value::fromDouble(static_cast<double>(newLen)));
    Rooted<Value> made{Value(bronze_construct(species.get().rawBits(), 1, block.data()))};
    if (!isSharedBuffer(made.get())) {
        return rtThrowTypeError("the species constructor did not return a SharedArrayBuffer");
    }
    if (made.get().rawBits() == self.get().rawBits()) {
        return rtThrowTypeError("the species constructor returned the same SharedArrayBuffer");
    }
    if (made.get().asObject<ArrayBufferHeader>()->byteLength < newLen) {
        return rtThrowTypeError("the species constructor returned a too-small SharedArrayBuffer");
    }
    return made.get();
}

// 25.2.5.6 SharedArrayBuffer.prototype.slice. Allocates a SHARED buffer, which
// is the one line that keeps this from being ArrayBuffer.prototype.slice: the
// brand has to survive the copy or `sab.slice(0).grow` disappears.
uint64_t sharedArrayBufferSlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!isSharedBuffer(self.get())) {
        return rtThrowTypeError(
                   "SharedArrayBuffer.prototype.slice called on non-SharedArrayBuffer")
            .rawBits();
    }
    const uint32_t len = self.get().asObject<ArrayBufferHeader>()->byteLength;
    uint32_t first = 0;
    if (args.count() > 0 && !args[0].isUndefined()) {
        first = relativeIndex(toInteger(rtToNumber(args[0])), len);
    }
    uint32_t final = len;
    if (args.count() > 1 && !args[1].isUndefined()) {
        final = relativeIndex(toInteger(rtToNumber(args[1])), len);
    }
    const uint32_t newLen = final > first ? final - first : 0;
    Rooted<Value> newBufVal{sharedSpeciesNew(self, newLen)};
    auto* oldBuf = self.get().asObject<ArrayBufferHeader>();
    auto* newBuf = newBufVal.get().asObject<ArrayBufferHeader>();
    if (newLen > 0) std::memcpy(newBuf->data(), oldBuf->data() + first, newLen);
    return newBufVal.get().rawBits();
}

// ---- 25.4 Atomics ---------------------------------------------------------

// The eight element kinds 25.4.3.1 admits, with what an operation needs to know
// about each. Uint8Clamped is NOT one of them (its store is a clamp, so a
// read-modify-write over it would not be a modular arithmetic at all) and
// neither are the three float kinds.
struct AtomicKind {
    uint32_t width;  // bytes
    bool isSigned;
    bool isBigInt;
};

bool atomicKindOf(ElementKind kind, AtomicKind& out) {
    switch (kind) {
        case ElementKind::Int8: out = {1, true, false}; return true;
        case ElementKind::Uint8: out = {1, false, false}; return true;
        case ElementKind::Int16: out = {2, true, false}; return true;
        case ElementKind::Uint16: out = {2, false, false}; return true;
        case ElementKind::Int32: out = {4, true, false}; return true;
        case ElementKind::Uint32: out = {4, false, false}; return true;
        case ElementKind::BigInt64: out = {8, true, true}; return true;
        case ElementKind::BigUint64: out = {8, false, true}; return true;
        default: return false;
    }
}

uint64_t maskFor(uint32_t width) {
    return width == 8 ? ~uint64_t{0} : ((uint64_t{1} << (width * 8)) - 1);
}

// The raw element bytes, zero-extended. Little-endian, as every other byte-level
// path in this runtime is (typed_array.cpp memcpys native types), and valid only
// until the next allocation.
uint64_t readRaw(TypedArrayHeader* view, uint32_t index, uint32_t width) {
    uint64_t bits = 0;
    std::memcpy(&bits, view->bytes() + static_cast<size_t>(index) * width, width);
    return bits;
}

void writeRaw(TypedArrayHeader* view, uint32_t index, uint32_t width, uint64_t bits) {
    std::memcpy(view->bytes() + static_cast<size_t>(index) * width, &bits, width);
}

// RawBytesToNumeric for the six Number kinds: sign-extend from the element's
// width when the kind is signed, then to double. Every value fits a double
// exactly at these widths, so this is not a rounding.
double numberOfRaw(uint64_t bits, uint32_t width, bool isSigned) {
    if (!isSigned) return static_cast<double>(bits & maskFor(width));
    const uint32_t shift = 64 - width * 8;
    return static_cast<double>(static_cast<int64_t>(bits << shift) >> shift);
}

// NumericToRawBytes for the same six: ToInt8/ToUint8/…/ToUint32, which is a
// modulo and never a clamp. A non-finite value is 0, as 7.1.5's
// ToIntegerOrInfinity chain makes it.
//
// The reduction is `fmod` (exact: 2^64 is a power of two and `integer` is
// already integral) followed by MAGNITUDE conversion and a two's-complement
// negation in the integer domain. Wrapping a negative into [0, 2^64) with
// `m += 2^64` in the double domain is what this first did and it is wrong: for
// -1 the mathematical answer 2^64 - 1 is not a double, so it rounds up to 2^64
// and the conversion below overflows to 0. Negating u64 bits cannot round.
uint64_t rawOfNumber(double integer, uint32_t width) {
    if (!std::isfinite(integer)) return 0;
    constexpr double kTwo64 = 18446744073709551616.0;
    constexpr double kTwo63 = 9223372036854775808.0;
    double m = std::fmod(integer, kTwo64);
    const bool negative = m < 0;
    if (negative) m = -m;
    uint64_t bits = m >= kTwo63 ? static_cast<uint64_t>(m - kTwo63) + (uint64_t{1} << 63)
                                : static_cast<uint64_t>(m);
    if (negative) bits = ~bits + 1;
    return bits & maskFor(width);
}

// 25.4.3.1 ValidateIntegerTypedArray + 25.4.3.2 ValidateAtomicAccess, in that
// order, with the receiver ROOTED because ToIndex on the index can run user
// code. Throws on a failed validation, and answers true otherwise.
bool validateAccess(const char* method, Rooted<Value>& view, Value indexVal, AtomicKind& kind,
                    uint32_t& index) {
    if (!isTypedArray(view.get())) {
        rtThrowTypeError(std::string("Atomics.") + method +
                         " called on a value that is not an integer typed array");
        return false;
    }
    auto* ta = view.get().asObject<TypedArrayHeader>();
    if (!atomicKindOf(ta->elementKind(), kind)) {
        rtThrowTypeError(std::string("Atomics.") + method + " does not accept a " + ta->kindName());
        return false;
    }
    if (ta->buffer.isObject() && ta->buffer.asObject<ArrayBufferHeader>()->isDetached()) {
        rtThrowTypeError("ArrayBuffer is detached");
        return false;
    }
    Rooted<Value> indexRoot{indexVal};
    if (!toIndex(indexRoot.get(), "index", 1, index)) return false;
    if (index >= view.get().asObject<TypedArrayHeader>()->length) {
        rtThrowRangeError("Invalid atomic access index");
        return false;
    }
    return true;
}

// The converted operand as raw element bytes. ToBigInt for the two 64-bit
// integer kinds and ToNumber-then-truncate for the other six (25.4.1.2 step 3),
// and either can run user code — so the caller re-derives the view afterwards.
// `converted` is what 25.4.3.11's `store` RETURNS, which is the value before
// the modulo, not the bytes.
bool operandRaw(Value value, const AtomicKind& kind, uint64_t& raw, Rooted<Value>& converted) {
    Rooted<Value> val{value};
    if (kind.isBigInt) {
        if (!rtBigIntToRawBits64(val.get(), raw)) return false;
        converted.set(val.get());
        return true;
    }
    const double num = rtToNumber(val.get());
    const double integer = toInteger(num);
    raw = rawOfNumber(integer, kind.width);
    converted.set(Value::fromDouble(integer));
    return true;
}

// One element as the JS value its kind reads as. ALLOCATES for a BigInt kind,
// so the bits are the argument and not the view.
Value valueOfRaw(uint64_t bits, const AtomicKind& kind) {
    if (kind.isBigInt) return rtBigIntFromRawBits64(bits, kind.isSigned);
    return Value::fromDouble(numberOfRaw(bits, kind.width, kind.isSigned));
}

uint64_t atomicsLoad(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> view{args[0]};
    AtomicKind kind{};
    uint32_t index = 0;
    if (!validateAccess("load", view, args[1], kind, index)) {
        return Value::fromUndefined().rawBits();
    }
    const uint64_t bits = readRaw(view.get().asObject<TypedArrayHeader>(), index, kind.width);
    return valueOfRaw(bits, kind).rawBits();
}

uint64_t atomicsStore(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> view{args[0]};
    AtomicKind kind{};
    uint32_t index = 0;
    if (!validateAccess("store", view, args[1], kind, index)) {
        return Value::fromUndefined().rawBits();
    }
    uint64_t raw = 0;
    Rooted<Value> converted{Value::fromUndefined()};
    if (!operandRaw(args[2], kind, raw, converted)) return Value::fromUndefined().rawBits();
    auto* live = view.get().asObject<TypedArrayHeader>();
    if (index >= live->length) return converted.get().rawBits();
    writeRaw(live, index, kind.width, raw);
    // 25.4.3.11 returns `v`, the CONVERTED value rather than the stored bytes:
    // `Atomics.store(u8, 0, 300)` answers 300 and stores 44.
    return converted.get().rawBits();
}

enum class Rmw { Add, Sub, And, Or, Xor, Exchange };

uint64_t applyRmw(Rmw op, uint64_t oldBits, uint64_t operand, uint32_t width) {
    const uint64_t mask = maskFor(width);
    switch (op) {
        case Rmw::Add: return (oldBits + operand) & mask;
        case Rmw::Sub: return (oldBits - operand) & mask;
        case Rmw::And: return (oldBits & operand) & mask;
        case Rmw::Or: return (oldBits | operand) & mask;
        case Rmw::Xor: return (oldBits ^ operand) & mask;
        case Rmw::Exchange: return operand & mask;
    }
    fatal("internal: unknown atomic read-modify-write operation");
}

// 25.4.1.2 AtomicReadModifyWrite, once. Every one of the six differs only in
// `applyRmw`, and all six return the OLD value.
template <Rmw Op>
uint64_t atomicsRmw(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    static constexpr const char* kNames[] = {"add", "sub", "and", "or", "xor", "exchange"};
    RootedArgs args(argc, argv);
    Rooted<Value> view{args[0]};
    AtomicKind kind{};
    uint32_t index = 0;
    if (!validateAccess(kNames[static_cast<size_t>(Op)], view, args[1], kind, index)) {
        return Value::fromUndefined().rawBits();
    }
    uint64_t operand = 0;
    Rooted<Value> converted{Value::fromUndefined()};
    if (!operandRaw(args[2], kind, operand, converted)) return Value::fromUndefined().rawBits();
    auto* live = view.get().asObject<TypedArrayHeader>();
    if (index >= live->length) return Value::fromUndefined().rawBits();
    const uint64_t oldBits = readRaw(live, index, kind.width);
    writeRaw(live, index, kind.width, applyRmw(Op, oldBits, operand, kind.width));
    // The read and the write are both done: `valueOfRaw` can allocate a BigInt
    // and the raw pointer above must not outlive it.
    return valueOfRaw(oldBits, kind).rawBits();
}

// 25.4.3.5 Atomics.compareExchange. The comparison is on the RAW BYTES, which
// is what makes `compareExchange(u8, 0, 300, 1)` succeed against a stored 44:
// 25.4.1.2 truncates the expected value before comparing it.
uint64_t atomicsCompareExchange(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> view{args[0]};
    AtomicKind kind{};
    uint32_t index = 0;
    if (!validateAccess("compareExchange", view, args[1], kind, index)) {
        return Value::fromUndefined().rawBits();
    }
    uint64_t expected = 0;
    uint64_t replacement = 0;
    Rooted<Value> converted{Value::fromUndefined()};
    if (!operandRaw(args[2], kind, expected, converted)) return Value::fromUndefined().rawBits();
    if (!operandRaw(args[3], kind, replacement, converted)) return Value::fromUndefined().rawBits();
    auto* live = view.get().asObject<TypedArrayHeader>();
    if (index >= live->length) return Value::fromUndefined().rawBits();
    const uint64_t oldBits = readRaw(live, index, kind.width);
    if (oldBits == (expected & maskFor(kind.width))) {
        writeRaw(live, index, kind.width, replacement & maskFor(kind.width));
    }
    return valueOfRaw(oldBits, kind).rawBits();
}

// 25.4.3.7 Atomics.isLockFree. The answer is the implementation's, and bronze's
// is the honest one for a single agent on the platforms it targets: every width
// a view can have is one native aligned access.
uint64_t atomicsIsLockFree(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const double size = toInteger(rtToNumber(args[0]));
    const bool lockFree = size == 1.0 || size == 2.0 || size == 4.0 || size == 8.0;
    return Value::fromBool(lockFree).rawBits();
}

using AtomicsFn = NativeMethod;

const AtomicsFn kAtomicsFunctions[] = {
    {"load", atomicsLoad, 2, 2},
    {"store", atomicsStore, 3, 3},
    {"add", atomicsRmw<Rmw::Add>, 3, 3},
    {"sub", atomicsRmw<Rmw::Sub>, 3, 3},
    {"and", atomicsRmw<Rmw::And>, 3, 3},
    {"or", atomicsRmw<Rmw::Or>, 3, 3},
    {"xor", atomicsRmw<Rmw::Xor>, 3, 3},
    {"exchange", atomicsRmw<Rmw::Exchange>, 3, 3},
    {"compareExchange", atomicsCompareExchange, 4, 4},
    {"isLockFree", atomicsIsLockFree, 1, 1},
};

// The three 25.4 operations that are not a memory access but an AGENT
// operation. `wait` blocks the calling agent until another one notifies it and
// `notify` wakes agents in a wait list — and bronze has one agent, no way to
// spawn a second, and no wait list for anything to be in.
//
// `notify` could legally answer 0 (no agent was woken, which is true), and that
// is deliberately NOT what it does: the loop that calls `notify` has a
// counterpart that calls `wait`, and `wait` cannot be answered at all — a
// spin-wait whose `notify` silently succeeds becomes an infinite loop instead of
// a diagnostic. Refusing all three puts the error at the first line of the
// protocol rather than at its deadlock.
const char* const kAtomicsUnimplemented[] = {
    "wait",
    "waitAsync",
    "notify",
};

void defineSharedGetter(Rooted<Value>& proto, const char* name, bronze_fn_code code,
                        const char* getterName) {
    Rooted<Value> key{rtMakeString(name)};
    Rooted<Value> getter{rtNativeFunction(code, 0, getterName, 0)};
    Rooted<Value> setter{Value::fromUndefined()};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), proto, key, getter, setter,
                                 /*enumerable=*/false);
}

// 25.2.5: the growable pair, and NOT `resizable`/`resize` — a
// SharedArrayBuffer has neither, nor `detached`, `transfer` or
// `transferToFixedLength`, because detaching memory another agent holds is
// not an operation the shared surface has.
const NativeMethod kSharedMethods[] = {
    {"grow", sharedArrayBufferGrow, 1, 1},
    {"slice", sharedArrayBufferSlice, 0, 2},
};

// `SharedArrayBuffer` and `SharedArrayBuffer.prototype`, on the terms
// builtin_array_buffer.cpp builds the plain pair.
void ensureSharedArrayBufferIntrinsics() {
    if (g_shared.proto.isObject()) return;

    Shape* protoShape = rtNewRootShape(rtObjectPrototype());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};
    g_shared.proto = proto.get();
    rtHeap().add_permanent_root(&g_shared.proto);

    // Arity 0: a variadic native must not be padded. `length` is 25.2.3's 1.
    Rooted<Value> ctor{rtNativeFunction(sharedArrayBufferCtor, 0, "SharedArrayBuffer", 1)};
    g_shared.ctor = ctor.get();
    rtHeap().add_permanent_root(&g_shared.ctor);
    {
        // 25.2.5.2, a DEFINITION so it does not write through.
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    rtDefineMethods(proto, kSharedMethods, std::size(kSharedMethods));
    defineSharedGetter(proto, "byteLength", sharedByteLengthGetter, "get byteLength");
    defineSharedGetter(proto, "growable", sharedGrowableGetter, "get growable");
    defineSharedGetter(proto, "maxByteLength", sharedMaxByteLengthGetter, "get maxByteLength");
    // 25.2.5.8: non-writable, configurable.
    rtDefineToStringTag(proto, "SharedArrayBuffer");
    // 25.2.4: no statics but 25.2.4.2's `@@species` accessor.
    rtDefineSpeciesGetter(ctor);

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // 25.2.4.1: non-writable, non-enumerable, non-configurable.
    fn->prototype_readonly = true;
    fn->native_base = NativeBase::SharedArrayBuffer;
    fn->instance_shape = rtNewRootShape(proto.get());
    g_shared.instanceShape = fn->instance_shape;
}

}  // namespace

Value rtSharedArrayBufferConstructor(const std::string& name) {
    if (name != "SharedArrayBuffer") return Value::fromUndefined();
    ensureSharedArrayBufferIntrinsics();
    return g_shared.ctor;
}

Shape* rtSharedArrayBufferInstanceShape() {
    ensureSharedArrayBufferIntrinsics();
    return g_shared.instanceShape;
}

Value rtNewSharedArrayBuffer(uint32_t byteLength, uint32_t maxByteLength) {
    return Value::fromObject(ArrayBufferHeader::createShared(
        rtHeap(), rtSharedArrayBufferInstanceShape(), byteLength, maxByteLength));
}

const char* rtSharedArrayBufferConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    // By CODE POINTER: identifying an intrinsic must never build one, because
    // callers of this sit on the property-miss ladder where an unexpected
    // allocation retires the box being looked up.
    if (fn.asObject<FunctionHeader>()->code == sharedArrayBufferCtor) return "SharedArrayBuffer";
    return nullptr;
}

Value rtAtomicsObject() {
    if (g_atomicsObject.isObject()) return g_atomicsObject;

    // Its own root shape, for the reason `Math` has one: a site reading
    // `Atomics.load` must not share a transition tree with every `{}`. It names
    // %Object.prototype% because 25.4.2 gives the namespace the ordinary
    // [[Prototype]]; with `undefined` there `Object.getPrototypeOf(Atomics)`
    // had no answer at all.
    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(rtObjectPrototype())))};
    obj.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;

    for (const AtomicsFn& fn : kAtomicsFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{rtNativeFunction(fn.code, fn.arity, fn.name, fn.length)};
        // 25.4: non-enumerable, like every namespace member — the default here
        // is an ordinary assignment's `enumerable: true`, which put all ten in
        // `Object.keys(Atomics)`.
        obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                    /*enumerable=*/false, /*defineOwn=*/true);
    }

    // 25.4.2: `Atomics[@@toStringTag]` is "Atomics", an own property.
    rtDefineToStringTag(obj, "Atomics");

    g_atomicsObject = obj.get();
    rtHeap().add_permanent_root(&g_atomicsObject);
    return g_atomicsObject;
}

bool rtAtomicsCheckMissingMember(Value obj, const std::string& key) {
    if (g_atomicsObject.isObject() && obj.rawBits() == g_atomicsObject.rawBits()) {
        rtCheckUnimplementedMember("Atomics", kAtomicsUnimplemented,
                                   sizeof(kAtomicsUnimplemented) / sizeof(kAtomicsUnimplemented[0]),
                                   key);
        return true;
    }
    return false;
}

}  // namespace bronze::runtime
