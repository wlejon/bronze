// `ArrayBuffer` (ECMA-262 25.1): the constructor, `isView`, and the members an
// instance answers — the size getters and the four prototype methods that
// mutate or copy the byte store. The nine views over a buffer are
// builtin_typed_array.cpp, the SHARED surface (25.2) is
// builtin_shared_memory.cpp, and the representation is typed_array.{h,cpp};
// builtin_typed_array_internal.h is the seam between them.

#include <algorithm>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/object.h"
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

uint64_t arrayBufferIsView(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    if (!v.isObject()) return Value::fromBool(false).rawBits();
    const uint16_t flags = v.asObject<HeapObjectHeader>()->flags;
    return Value::fromBool(flags == TypedArrayHeader::kFlags || flags == DataViewHeader::kFlags)
        .rawBits();
}

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

}  // namespace

// Arity 0: a variadic native must not be padded. `length` is 25.1.4's 1.
Value rtArrayBufferConstructor(const std::string& name) {
    if (name == "ArrayBuffer") return rtNativeFunction(arrayBufferCtor, 0, "ArrayBuffer", 1);
    return Value::fromUndefined();
}

const char* rtArrayBufferConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    return fn.asObject<FunctionHeader>()->code == arrayBufferCtor ? "ArrayBuffer" : nullptr;
}

bool rtArrayBufferStatic(Value fn, const std::string& key, Value& out) {
    if (!rtArrayBufferConstructorName(fn)) return false;
    if (key == "isView") {
        out = rtNativeFunction(arrayBufferIsView, 1, "isView", 1);
        return true;
    }
    return false;
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
    if (key == "resize") return rtNativeFunction(arrayBufferResize, 1, "resize", 1);
    if (key == "transfer") return rtNativeFunction(arrayBufferTransfer, 0, "transfer", 0);
    if (key == "transferToFixedLength") {
        return rtNativeFunction(arrayBufferTransferToFixedLength, 0, "transferToFixedLength", 0);
    }
    if (key == "slice") return rtNativeFunction(arrayBufferSlice, 0, "slice", 2);
    if (key == "constructor") return rtArrayBufferConstructor("ArrayBuffer");
    return Value::fromUndefined();
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
