// `ArrayBuffer` (ECMA-262 25.1): the constructor, `ArrayBuffer.prototype`
// with its four getters and four methods (25.1.6), `isView` (25.1.5.1), and
// the allocations the rest of the runtime makes on the intrinsic's behalf. The
// views over a buffer are builtin_typed_array.cpp, the SHARED surface (25.2)
// is builtin_shared_memory.cpp, and the representation is typed_array.{h,cpp};
// builtin_typed_array_internal.h is the seam between them.
//
// A buffer's bytes are INLINE in its header, so their count has to be known
// at allocation — and 25.1.4.1 step 1 makes that count an argument the
// constructor converts (ToIndex, whose `valueOf` is user code) AFTER
// OrdinaryCreateFromConstructor has already run. bronze's construction
// allocates before any body runs (runtime/native_base.h), so the object
// `bronze_construct` hands in is a zero-byte PLACEHOLDER carrying the
// [[Prototype]] NewTarget derived: the body below reads that prototype off it,
// allocates the real buffer with the same shape, and returns it. A returned
// object IS the result of `new` (13.3.5.1 / 10.2.2 step 10.a), so a derived
// constructor's `this` is rebound to the real buffer by `super()`, and the
// placeholder is garbage the moment the body returns.

#include <algorithm>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
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

// The intrinsic and its witnesses: the shape `ArrayBuffer.prototype` was left
// with, the slot its `constructor` sits in, and the constructor's box shape —
// what `slice` compares to skip 7.3.22's two reads (rt_receivers.h).
struct BufferIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Shape* instanceShape = nullptr;
    Shape* protoPristineShape = nullptr;
    Shape* ctorBoxPristineShape = nullptr;
    uint32_t constructorSlot = 0;
};

thread_local BufferIntrinsics g_buffer;

void ensureArrayBufferIntrinsics();

bool isPlainBuffer(Value v) { return isBuffer(v) && !v.asObject<ArrayBufferHeader>()->isShared(); }

// 25.1.6's getters and methods open with RequireInternalSlot(O,
// [[ArrayBufferData]]) and then refuse a SharedArrayBuffer (step 3 of each):
// a TypeError, never a lookup.
bool requireBuffer(Value v, const char* member) {
    if (isPlainBuffer(v)) return true;
    rtThrowTypeError(std::string("ArrayBuffer.prototype.") + member +
                     " called on incompatible receiver");
    return false;
}

// ---- the constructor (25.1.4.1) -------------------------------------------------

uint64_t arrayBufferCtor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    // Step 1: NewTarget undefined is a TypeError, witnessed by the receiver
    // being the placeholder the running construction allocated. The brand is
    // asked too: `Reflect.construct(ArrayBuffer, [8], SharedArrayBuffer)`
    // must not fill a shared placeholder.
    if (!rtIsNativeConstructReceiver(receiver.get()) || !isPlainBuffer(receiver.get())) {
        return rtThrowTypeError("Constructor ArrayBuffer requires 'new'").rawBits();
    }
    uint32_t byteLength = 0;
    if (!toIndex(args[0], "array buffer", 1, byteLength)) return Value::fromUndefined().rawBits();
    if (!checkAllocatable(byteLength)) return Value::fromUndefined().rawBits();

    if (args.count() > 1 && args[1].isObject()) {
        Rooted<Value> opts{args[1]};
        Rooted<Value> mblKey{rtMakeString("maxByteLength")};
        Rooted<Value> mblVal{
            Value(bronze_elem_get(opts.get().rawBits(), mblKey.get().rawBits()))};
        if (!mblVal.get().isUndefined()) {
            uint32_t maxByteLength = 0;
            if (!toIndex(mblVal.get(), "maxByteLength", 1, maxByteLength)) {
                return Value::fromUndefined().rawBits();
            }
            if (maxByteLength < byteLength) {
                return rtThrowRangeError("maxByteLength must be >= byteLength").rawBits();
            }
            if (!checkAllocatable(maxByteLength)) return Value::fromUndefined().rawBits();
            // Read AFTER every conversion above: each can allocate and move the
            // placeholder, and the shape is what carries NewTarget's answer.
            Shape* shape = receiver.get().asObject<ArrayBufferHeader>()->object.shape;
            return Value::fromObject(ArrayBufferHeader::createResizable(rtHeap(), shape, byteLength,
                                                                        maxByteLength))
                .rawBits();
        }
    }
    Shape* shape = receiver.get().asObject<ArrayBufferHeader>()->object.shape;
    return Value::fromObject(ArrayBufferHeader::create(rtHeap(), shape, byteLength)).rawBits();
}

// 25.1.5.1 ArrayBuffer.isView.
uint64_t arrayBufferIsView(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    if (!v.isObject()) return Value::fromBool(false).rawBits();
    const uint16_t flags = v.asObject<HeapObjectHeader>()->flags;
    return Value::fromBool(flags == TypedArrayHeader::kFlags || flags == DataViewHeader::kFlags)
        .rawBits();
}

// ---- the getters (25.1.6.1–.4) -------------------------------------------------

uint64_t byteLengthGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireBuffer(self, "byteLength")) return Value::fromUndefined().rawBits();
    const auto* buf = self.asObject<ArrayBufferHeader>();
    return Value::fromDouble(buf->isDetached() ? 0.0 : static_cast<double>(buf->byteLength))
        .rawBits();
}

uint64_t maxByteLengthGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireBuffer(self, "maxByteLength")) return Value::fromUndefined().rawBits();
    const auto* buf = self.asObject<ArrayBufferHeader>();
    return Value::fromDouble(buf->isDetached() ? 0.0 : static_cast<double>(buf->maxByteLength))
        .rawBits();
}

uint64_t resizableGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireBuffer(self, "resizable")) return Value::fromUndefined().rawBits();
    const auto* buf = self.asObject<ArrayBufferHeader>();
    return Value::fromBool(buf->isDetached() ? false : buf->isResizable()).rawBits();
}

uint64_t detachedGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireBuffer(self, "detached")) return Value::fromUndefined().rawBits();
    return Value::fromBool(self.asObject<ArrayBufferHeader>()->isDetached()).rawBits();
}

// ---- the methods (25.1.6.5–.8) --------------------------------------------------

// 25.1.6.5 ArrayBuffer.prototype.resize
uint64_t arrayBufferResize(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireBuffer(self.get(), "resize")) return Value::fromUndefined().rawBits();
    if (!self.get().asObject<ArrayBufferHeader>()->isResizable()) {
        return rtThrowTypeError("Cannot resize a non-resizable ArrayBuffer").rawBits();
    }
    uint32_t newLen = 0;
    // ToIndex can run user code: the header is re-derived after it, and the
    // detach test (step 5) sits after it too, as the clause orders them.
    if (!toIndex(args[0], "byte length", 1, newLen)) return Value::fromUndefined().rawBits();
    auto* buf = self.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("Cannot resize a detached ArrayBuffer").rawBits();
    }
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
    closeOrReopenViews(rtHeap(), self);
    return Value::fromUndefined().rawBits();
}

// 25.1.3.16 ArrayBufferCopyAndDetach, the shared half of `transfer` and
// `transferToFixedLength`: the new buffer is always the INTRINSIC's (step 14
// allocates from %ArrayBuffer%, never a species).
uint64_t copyAndDetach(Rooted<Value>& self, Value lengthArg, bool preserveResizability,
                       const char* member) {
    if (!requireBuffer(self.get(), member)) return Value::fromUndefined().rawBits();
    uint32_t newLen = self.get().asObject<ArrayBufferHeader>()->byteLength;
    if (!lengthArg.isUndefined()) {
        if (!toIndex(lengthArg, "byte length", 1, newLen)) return Value::fromUndefined().rawBits();
    }
    if (!checkAllocatable(newLen)) return Value::fromUndefined().rawBits();
    auto* buf = self.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("Cannot transfer a detached ArrayBuffer").rawBits();
    }
    const bool resizable = preserveResizability && buf->isResizable();
    const uint32_t maxByteLen = buf->maxByteLength;
    const uint32_t oldLen = buf->byteLength;
    if (resizable && newLen > maxByteLen) {
        return rtThrowRangeError("newByteLength exceeds maxByteLength").rawBits();
    }

    Rooted<Value> newBufVal{resizable ? rtNewResizableArrayBuffer(newLen, maxByteLen)
                                      : rtNewArrayBuffer(newLen)};
    auto* oldBuf = self.get().asObject<ArrayBufferHeader>();
    auto* newBuf = newBufVal.get().asObject<ArrayBufferHeader>();
    std::memcpy(newBuf->data(), oldBuf->data(), std::min(oldLen, newLen));
    oldBuf->setDetached();
    // The detach closes every view over the old buffer FOREVER (its
    // byteLength is 0 from here on); their length fields carry the truth,
    // so they are zeroed here, at the mutation, rather than checked on every
    // element access.
    closeOrReopenViews(rtHeap(), self);
    return newBufVal.get().rawBits();
}

// 25.1.6.7 ArrayBuffer.prototype.transfer
uint64_t arrayBufferTransfer(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    return copyAndDetach(self, args[0], /*preserveResizability=*/true, "transfer");
}

// 25.1.6.8 ArrayBuffer.prototype.transferToFixedLength
uint64_t arrayBufferTransferToFixedLength(uint64_t, uint64_t thisBits, uint32_t argc,
                                          const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    return copyAndDetach(self, args[0], /*preserveResizability=*/false,
                         "transferToFixedLength");
}

// 25.1.6.6 step 14: SpeciesConstructor(O, %ArrayBuffer%) and `new ctor(len)`,
// with the checks of steps 16-21 on what came back. A buffer whose chain is
// pristine skips the two `Get`s and allocates the intrinsic directly.
Value bufferSpeciesNew(Rooted<Value>& self, uint32_t newLen) {
    const auto* buf = self.get().asObject<ArrayBufferHeader>();
    const auto* proto = g_buffer.proto.asObject<ObjectHeader>();
    const bool pristine =
        buf->object.shape == g_buffer.instanceShape &&
        proto->shape == g_buffer.protoPristineShape &&
        proto->getSlot(g_buffer.constructorSlot).rawBits() == g_buffer.ctor.rawBits() &&
        g_buffer.ctor.asObject<FunctionHeader>()->properties.asObject<ObjectHeader>()->shape ==
            g_buffer.ctorBoxPristineShape;
    if (pristine) return rtNewArrayBuffer(newLen);

    Rooted<Value> ctorKey{rtMakeString("constructor")};
    Rooted<Value> ctor{Value(bronze_elem_get(self.get().rawBits(), ctorKey.get().rawBits()))};
    if (ctor.get().isUndefined()) ctor.set(g_buffer.ctor);
    if (!ctor.get().isObject()) {
        return rtThrowTypeError("the constructor of this ArrayBuffer is not an object");
    }
    Rooted<Value> speciesKey{Value::fromSymbol(rtSymbolSpecies())};
    Rooted<Value> species{
        Value(bronze_elem_get(ctor.get().rawBits(), speciesKey.get().rawBits()))};
    if (species.get().isUndefined() || species.get().isNull()) species.set(g_buffer.ctor);
    if (!rtIsConstructorValue(species.get())) {
        return rtThrowTypeError("[Symbol.species] of this ArrayBuffer is not a constructor");
    }
    RootedBlock block(1);
    block.set(0, Value::fromDouble(static_cast<double>(newLen)));
    Rooted<Value> made{Value(bronze_construct(species.get().rawBits(), 1, block.data()))};
    if (!isPlainBuffer(made.get())) {
        return rtThrowTypeError("the species constructor did not return an ArrayBuffer");
    }
    if (made.get().asObject<ArrayBufferHeader>()->isDetached()) {
        return rtThrowTypeError("the species constructor returned a detached ArrayBuffer");
    }
    if (made.get().rawBits() == self.get().rawBits()) {
        return rtThrowTypeError("the species constructor returned the same ArrayBuffer");
    }
    if (made.get().asObject<ArrayBufferHeader>()->byteLength < newLen) {
        return rtThrowTypeError("the species constructor returned a too-small ArrayBuffer");
    }
    // Step 22: the SOURCE may have been detached by the constructor.
    if (self.get().asObject<ArrayBufferHeader>()->isDetached()) {
        return rtThrowTypeError("Cannot slice a detached ArrayBuffer");
    }
    return made.get();
}

// 25.1.6.6 ArrayBuffer.prototype.slice
uint64_t arrayBufferSlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireBuffer(self.get(), "slice")) return Value::fromUndefined().rawBits();
    if (self.get().asObject<ArrayBufferHeader>()->isDetached()) {
        return rtThrowTypeError("Cannot slice a detached ArrayBuffer").rawBits();
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
    Rooted<Value> newBufVal{bufferSpeciesNew(self, newLen)};
    auto* oldBuf = self.get().asObject<ArrayBufferHeader>();
    auto* newBuf = newBufVal.get().asObject<ArrayBufferHeader>();
    // Step 23 measures the source AGAIN: a species constructor can shrink it.
    const uint32_t copyable = oldBuf->byteLength > first
                                  ? std::min(newLen, oldBuf->byteLength - first)
                                  : 0;
    if (copyable > 0) std::memcpy(newBuf->data(), oldBuf->data() + first, copyable);
    return newBufVal.get().rawBits();
}

// ---- assembling the intrinsic ---------------------------------------------------

void defineGetter(Rooted<Value>& proto, const char* name, bronze_fn_code code,
                  const char* getterName) {
    Rooted<Value> key{rtMakeString(name)};
    Rooted<Value> getter{rtNativeFunction(code, 0, getterName, 0)};
    Rooted<Value> setter{Value::fromUndefined()};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), proto, key, getter, setter,
                                 /*enumerable=*/false);
}

const NativeMethod kBufferMethods[] = {
    {"resize", arrayBufferResize, 1, 1},
    {"slice", arrayBufferSlice, 0, 2},
    {"transfer", arrayBufferTransfer, 0, 0},
    {"transferToFixedLength", arrayBufferTransferToFixedLength, 0, 0},
};

void ensureArrayBufferIntrinsics() {
    if (g_buffer.proto.isObject()) return;

    Shape* protoShape = rtNewRootShape(rtObjectPrototype());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};
    g_buffer.proto = proto.get();
    rtHeap().add_permanent_root(&g_buffer.proto);

    // Arity 0: a variadic native must not be padded. `length` is 25.1.4's 1.
    Rooted<Value> ctor{rtNativeFunction(arrayBufferCtor, 0, "ArrayBuffer", 1)};
    g_buffer.ctor = ctor.get();
    rtHeap().add_permanent_root(&g_buffer.ctor);
    {
        // 25.1.6.2, a DEFINITION so it does not write through.
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
        PropertyInfo info;
        proto.get().asObject<ObjectHeader>()->shape->lookupProperty(
            PropertyKey::forString(key.get().asString<StringHeader>()), info);
        g_buffer.constructorSlot = info.slot;
    }
    rtDefineMethods(proto, kBufferMethods, std::size(kBufferMethods));
    defineGetter(proto, "byteLength", byteLengthGetter, "get byteLength");
    defineGetter(proto, "detached", detachedGetter, "get detached");
    defineGetter(proto, "maxByteLength", maxByteLengthGetter, "get maxByteLength");
    defineGetter(proto, "resizable", resizableGetter, "get resizable");
    // 25.1.6.9: non-writable, configurable.
    rtDefineToStringTag(proto, "ArrayBuffer");

    // 25.1.5: `isView` and 25.1.5.3's `@@species` accessor, ordinary own
    // properties of the constructor's box — where a subclass reaches them by
    // the chain `extends` builds.
    rtEnsureFunctionProperties(ctor);
    {
        Rooted<Value> box{ctor.get().asObject<FunctionHeader>()->properties};
        const NativeMethod statics[] = {{"isView", arrayBufferIsView, 1, 1}};
        rtDefineMethods(box, statics, std::size(statics));
        rtDefineSpeciesGetter(ctor);
        g_buffer.ctorBoxPristineShape = box.get().asObject<ObjectHeader>()->shape;
    }

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // 25.1.5.2: non-writable, non-enumerable, non-configurable.
    fn->prototype_readonly = true;
    fn->native_base = NativeBase::ArrayBuffer;
    fn->instance_shape = rtNewRootShape(proto.get());
    g_buffer.instanceShape = fn->instance_shape;
    g_buffer.protoPristineShape = proto.get().asObject<ObjectHeader>()->shape;
}

}  // namespace

Value rtArrayBufferConstructor(const std::string& name) {
    if (name != "ArrayBuffer") return Value::fromUndefined();
    ensureArrayBufferIntrinsics();
    return g_buffer.ctor;
}

// By CODE POINTER: identifying an intrinsic must never build one.
const char* rtArrayBufferConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    return fn.asObject<FunctionHeader>()->code == arrayBufferCtor ? "ArrayBuffer" : nullptr;
}

Shape* rtArrayBufferInstanceShape() {
    ensureArrayBufferIntrinsics();
    return g_buffer.instanceShape;
}

Value rtNewArrayBuffer(uint32_t byteLength) {
    return Value::fromObject(
        ArrayBufferHeader::create(rtHeap(), rtArrayBufferInstanceShape(), byteLength));
}

Value rtNewResizableArrayBuffer(uint32_t byteLength, uint32_t maxByteLength) {
    return Value::fromObject(ArrayBufferHeader::createResizable(
        rtHeap(), rtArrayBufferInstanceShape(), byteLength, maxByteLength));
}

}  // namespace bronze::runtime
