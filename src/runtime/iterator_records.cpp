// The iterator protocol: the key `Symbol.iterator` denotes, the prototype chain
// every iterator object hangs from, and the two walks a for-of can take.
//
// Two walks live here and they are deliberately not two mechanisms. The FAST
// kinds — an array, a string, a typed array, a Map, a Set — step a cursor the
// runtime owns: no iterator object, no result object, no call into user code
// per element, which is what the index walk bought and what this must not give
// back. The PROTOCOL kind is the general answer: read `[Symbol.iterator]`, call it,
// call `next` until `done`, and call `return` if the loop is abandoned. A
// user-defined iterable is the whole reason it exists.
//
// Which one a value gets is decided ONCE, at open time, and recorded in the
// record — so the loop's step is a switch on an integer rather than a
// re-derivation per element.

#include <bit>
#include <cstddef>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/async_generator.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/generator.h"
#include "runtime/iterator.h"
#include "runtime/iterator_internal.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/promise.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/symbol.h"
#include "runtime/tls_block.h"
#include "runtime/typed_array.h"

namespace bronze {

static_assert(offsetof(IterRecordHeader, target) == BRONZE_ABI_ITER_TARGET_OFFSET);
static_assert(offsetof(IterRecordHeader, nextFn) == BRONZE_ABI_ITER_NEXTFN_OFFSET);
static_assert(offsetof(IterRecordHeader, current) == BRONZE_ABI_ITER_CURRENT_OFFSET);
static_assert(offsetof(IterRecordHeader, cursor) == BRONZE_ABI_ITER_CURSOR_OFFSET);
static_assert(offsetof(IterRecordHeader, kind) == BRONZE_ABI_ITER_KIND_OFFSET);
static_assert(offsetof(IterRecordHeader, done) == BRONZE_ABI_ITER_DONE_OFFSET);
static_assert(sizeof(IterRecordHeader) == BRONZE_ABI_ITER_RECORD_BYTES);
static_assert(IterRecordHeader::kFlags == BRONZE_ABI_OBJ_FLAGS_ITERATOR);
static_assert(IterRecordHeader::SetValues < IterRecordHeader::Protocol);
static_assert(IterRecordHeader::MapIterator > IterRecordHeader::Protocol);
static_assert(IterRecordHeader::ArrayIterator > IterRecordHeader::Protocol);
static_assert(IterRecordHeader::Protocol == 5);
static_assert(BRONZE_ABI_ITER_KIND_OWNED_LIMIT_BITS == 0x4014000000000000ull);  // 5.0
static_assert(std::bit_cast<uint64_t>(static_cast<double>(IterRecordHeader::MapEntries)) ==
              BRONZE_ABI_ITER_KIND_MAP_ENTRIES_BITS);
static_assert(std::bit_cast<uint64_t>(static_cast<double>(IterRecordHeader::MapIterator)) ==
              BRONZE_ABI_ITER_KIND_MAP_ITERATOR_BITS);

IterRecordHeader* IterRecordHeader::create(Heap& heap, uint32_t kind) {
    HeapObjectHeader* raw =
        heap.allocate(sizeof(IterRecordHeader) - sizeof(HeapObjectHeader), Tag::Object);
    auto* rec = reinterpret_cast<IterRecordHeader*>(raw);
    rec->header.flags = kFlags;
    rec->target = Value::fromUndefined();
    rec->nextFn = Value::fromUndefined();
    rec->current = Value::fromUndefined();
    rec->cursor = Value::fromDouble(0.0);
    rec->kind = Value::fromDouble(static_cast<double>(kind));
    rec->done = Value::fromBool(false);
    return rec;
}

}  // namespace bronze

namespace bronze::runtime {

namespace {

constexpr uint16_t kHighSurrogateFirst = 0xD800;
constexpr uint16_t kHighSurrogateLast = 0xDBFF;
constexpr uint16_t kLowSurrogateFirst = 0xDC00;
constexpr uint16_t kLowSurrogateLast = 0xDFFF;

bool isSurrogatePair(uint16_t high, uint16_t low) {
    return high >= kHighSurrogateFirst && high <= kHighSurrogateLast &&
           low >= kLowSurrogateFirst && low <= kLowSurrogateLast;
}

Value iteratorMethodOf(Value v) {
    if (!v.isObject()) return Value::fromUndefined();
    if (v.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) {
        return proxyMethodOf(v, rtIteratorKey());
    }
    if (!HeapKind::carriesShape(v.asObject<HeapObjectHeader>()->flags)) {
        return Value::fromUndefined();
    }
    Rooted<Value> objRoot{v};
    Rooted<Value> keyRoot{rtIteratorKey()};
    return objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), keyRoot);
}

bool pristineBuiltinIterator(Value v, IteratorProto kind, bronze_fn_code nextCode) {
    if (!rtIsIteratorObject(v, kind)) return false;
    auto* obj = v.asObject<ObjectHeader>();
    PropertyInfo info;
    if (!obj->shape || !obj->shape->lookupProperty(PropertyKey::forString(keyNext()), info)) {
        return false;
    }
    if (info.accessor) return false;
    const Value next = obj->getSlot(info.slot);
    if (!next.isObject() || next.asObject<HeapObjectHeader>()->flags != HeapKind::Function ||
        next.asObject<FunctionHeader>()->code != nextCode) {
        return false;
    }
    const Value hook = iteratorMethodOf(v);
    return hook.isObject() && hook.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           hook.asObject<FunctionHeader>()->code == iteratorProtoSelf;
}

void openProtocolFromMethod(Rooted<Value>& recRoot, Rooted<Value>& srcRoot,
                            Rooted<Value>& method) {
    Rooted<Value> iter{callMethod(method, srcRoot)};
    if (rtExceptionPending()) return;
    if (!iter.get().isObject()) {
        rtThrowTypeError("the result of Symbol.iterator is not an object");
        return;
    }
    Rooted<Value> next{namedProp(iter.get(), keyNext())};
    if (rtExceptionPending()) return;
    if (!isCallable(next.get())) {
        rtThrowTypeError("the iterator has no `next` method");
        return;
    }
    auto* rec = recRoot.get().asObject<IterRecordHeader>();
    rec->kind = Value::fromDouble(static_cast<double>(IterRecordHeader::Protocol));
    rec->target = iter.get();
    rec->nextFn = next.get();
}

void openProtocol(Rooted<Value>& recRoot, Rooted<Value>& srcRoot) {
    Rooted<Value> method{iteratorMethodOf(srcRoot.get())};
    if (rtExceptionPending()) return;
    if (!isCallable(method.get())) {
        rtThrowTypeError(rtIterableKindName(srcRoot.get()) + " is not iterable");
        return;
    }
    openProtocolFromMethod(recRoot, srcRoot, method);
}

bool stepFast(IterRecordHeader* rec) {
    const uint32_t i = rec->cursorOf();
    switch (rec->kindOf()) {
        case IterRecordHeader::Array: {
            auto* arr = rec->target.asObject<ArrayHeader>();
            if (i >= arr->length) return false;
            rec->current = arr->getElem(i);
            rec->cursor = Value::fromDouble(static_cast<double>(i + 1));
            return true;
        }
        case IterRecordHeader::TypedArray: {
            auto* view = rec->target.asObject<TypedArrayHeader>();
            if (i >= view->length) {
                auto* buf = view->buffer.asObject<ArrayBufferHeader>();
                if (buf->isDetached()) {
                    rtThrowTypeError("ArrayBuffer is detached");
                } else if (view->isOutOfBounds()) {
                    rtThrowTypeError("TypedArray is out of bounds of its ArrayBuffer");
                }
                return false;
            }
            if (isBigIntElementKind(view->elementKind())) {
                const uint64_t bits = view->rawBits64(i);
                const bool isSigned = view->elementKind() == ElementKind::BigInt64;
                Rooted<Value> recRoot{Value::fromObject(rec)};
                Rooted<Value> elem{rtBigIntFromRawBits64(bits, isSigned)};
                auto* live = recRoot.get().asObject<IterRecordHeader>();
                live->current = elem.get();
                live->cursor = Value::fromDouble(static_cast<double>(i + 1));
                return true;
            }
            rec->current = Value::fromDouble(view->get(i));
            rec->cursor = Value::fromDouble(static_cast<double>(i + 1));
            return true;
        }
        case IterRecordHeader::SetValues:
        case IterRecordHeader::MapEntries: {
            auto* map = rec->target.asObject<MapHeader>();
            uint32_t slot = i;
            while (slot < map->used() && !map->liveAt(slot)) ++slot;
            if (slot >= map->used()) return false;
            rec->cursor = Value::fromDouble(static_cast<double>(slot + 1));
            rec->current = map->keyAt(slot);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

std::string rtIterableKindName(Value v) {
    if (v.isNumber()) return "a number";
    if (v.isString()) return "a string";
    if (v.isBool()) return "a boolean";
    if (v.isNull()) return "null";
    if (v.isUndefined()) return "undefined";
    if (!v.isObject()) return "a value";
    switch (v.asObject<HeapObjectHeader>()->flags) {
        case 2: return "a function";
        case ArrayBufferHeader::kFlags: return "an ArrayBuffer";
        case DataViewHeader::kFlags: return "a DataView";
        default: return "an object";
    }
}

Value rtOpenIterator(Value source) {
    Rooted<Value> srcRoot{source};
    uint32_t kind = IterRecordHeader::Protocol;
    if (source.isString()) {
        kind = IterRecordHeader::String;
    } else if (source.isObject()) {
        switch (source.asObject<HeapObjectHeader>()->flags) {
            case 1: kind = IterRecordHeader::Array; break;
            case TypedArrayHeader::kFlags: {
                if (rtTypedArrayIteratorPristine(source.asObject<TypedArrayHeader>())) {
                    kind = IterRecordHeader::TypedArray;
                    break;
                }
                Rooted<Value> method{iteratorMethodOf(srcRoot.get())};
                if (!rtExceptionPending() && rtIsIntrinsicTypedArrayIterator(method.get())) {
                    kind = IterRecordHeader::TypedArray;
                    break;
                }
                if (rtExceptionPending() || !isCallable(method.get())) {
                    if (!rtExceptionPending()) {
                        rtThrowTypeError(rtIterableKindName(srcRoot.get()) + " is not iterable");
                    }
                    return Value::fromObject(
                        IterRecordHeader::create(rtHeap(), IterRecordHeader::Protocol));
                }
                return rtGetIteratorFromMethod(srcRoot, method);
            }
            case BRONZE_ABI_OBJ_FLAGS_PLAIN:
                if (rtIsMapOrSet(source)) {
                    const bool isSet = rtIsSetKind(source);
                    Rooted<Value> method{iteratorMethodOf(srcRoot.get())};
                    if (!rtExceptionPending() &&
                        rtIsIntrinsicCollectionIterator(method.get(), isSet)) {
                        kind = isSet ? IterRecordHeader::SetValues : IterRecordHeader::MapEntries;
                        break;
                    }
                    if (rtExceptionPending() || !isCallable(method.get())) {
                        if (!rtExceptionPending()) {
                            rtThrowTypeError(rtIterableKindName(srcRoot.get()) +
                                             " is not iterable");
                        }
                        return Value::fromObject(
                            IterRecordHeader::create(rtHeap(), IterRecordHeader::Protocol));
                    }
                    return rtGetIteratorFromMethod(srcRoot, method);
                }
                if (pristineBuiltinIterator(source, IteratorProto::Map, rtMapIteratorNextCode()) ||
                    pristineBuiltinIterator(source, IteratorProto::Set, rtMapIteratorNextCode())) {
                    kind = IterRecordHeader::MapIterator;
                } else if (pristineBuiltinIterator(source, IteratorProto::Array,
                                                   rtArrayIteratorNextCode())) {
                    kind = IterRecordHeader::ArrayIterator;
                }
                break;
            default: break;
        }
    }

    Rooted<Value> recRoot{Value::fromObject(IterRecordHeader::create(rtHeap(), kind))};
    if (kind == IterRecordHeader::Protocol) {
        openProtocol(recRoot, srcRoot);
    } else {
        recRoot.get().asObject<IterRecordHeader>()->target = srcRoot.get();
    }
    return recRoot.get();
}

Value rtGetIteratorFromMethod(Rooted<Value>& source, Rooted<Value>& method) {
    Rooted<Value> recRoot{
        Value::fromObject(IterRecordHeader::create(rtHeap(), IterRecordHeader::Protocol))};
    openProtocolFromMethod(recRoot, source, method);
    return recRoot.get();
}

}  // namespace bronze::runtime

namespace bronze::runtime {

namespace {

// 27.1.6 CreateAsyncFromSyncIterator, without the wrapper object: `for await`
// over a sync iterable holds the sync record, and each step does what the
// wrapper's `next` (27.1.6.2.1) and AsyncFromSyncIteratorContinuation
// (27.1.6.4) would. Nothing a program can reach ever sees the wrapper, so
// building one per loop would be an allocation with no observer.
namespace AsyncFromSyncSlot {
enum : uint32_t { Promise, Record, kCount };
}

Value makeIterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> resObj{Value(bronze_create_object())};
    Rooted<Value> keyValue{rtMakeString("value")};
    resObj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), keyValue, value);
    Rooted<Value> keyDone{rtMakeString("done")};
    Rooted<Value> doneVal{Value::fromBool(done)};
    resObj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), keyDone, doneVal);
    return resObj.get();
}

// IfAbruptRejectPromise: the pending exception becomes the rejection.
void rejectWithPending(Rooted<Value>& promise) {
    Rooted<Value> reason{Value(rtTls()->exception_cell)};
    rtClearException();
    rtRejectPromise(promise, reason);
}

// 27.1.6.4 step 8, the fulfilled half: `{ value: v, done: false }`.
uint64_t asyncFromSyncOnValue(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> state{Value(env)};
    Rooted<Value> value{argc > 0 ? Value(argv[0]) : Value::fromUndefined()};
    Rooted<Value> result{makeIterResult(value, false)};
    Rooted<Value> promise{
        state.get().asObject<ObjectHeader>()->internalSlot(AsyncFromSyncSlot::Promise)};
    rtResolvePromise(promise, result);
    return Value::fromUndefined().rawBits();
}

// 27.1.6.4 step 10-11 (ES2025's closeOnRejection): a value that REJECTS closes
// the sync iterator — its `return` runs, and anything that throws is discarded
// in favour of the rejection, as IteratorClose with a throw completion does —
// and then rejects the step, which the loop throws.
uint64_t asyncFromSyncOnRejected(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> state{Value(env)};
    Rooted<Value> reason{argc > 0 ? Value(argv[0]) : Value::fromUndefined()};
    Rooted<Value> record{
        state.get().asObject<ObjectHeader>()->internalSlot(AsyncFromSyncSlot::Record)};
    bronze_iter_close(record.get().rawBits(), /*suppress=*/true);
    if (rtExceptionPending()) rtClearException();
    Rooted<Value> promise{
        state.get().asObject<ObjectHeader>()->internalSlot(AsyncFromSyncSlot::Promise)};
    rtRejectPromise(promise, reason);
    return Value::fromUndefined().rawBits();
}

// One step, answered as a promise for the iterator result. The sync step
// itself is not wrapped: its throw is the iterator's own, so it rejects the
// step without closing anything (27.1.6.2.1 step 5, IfAbruptRejectPromise).
Value asyncFromSyncNext(Rooted<Value>& recRoot) {
    Rooted<Value> promise{rtNewPromise()};
    const bool more = bronze_iter_step(recRoot.get().rawBits());
    if (rtExceptionPending()) {
        rejectWithPending(promise);
        return promise.get();
    }
    if (!more) {
        Rooted<Value> undef{Value::fromUndefined()};
        Rooted<Value> result{makeIterResult(undef, true)};
        rtResolvePromise(promise, result);
        return promise.get();
    }
    Rooted<Value> value{Value(bronze_iter_value(recRoot.get().rawBits()))};
    // PromiseResolve(%Promise%, value): a thenable's `then` getter may throw,
    // and that too closes the iterator (27.1.6.4 step 6).
    Rooted<Value> wrapper{rtPromiseResolveValue(value)};
    if (rtExceptionPending()) {
        bronze_iter_close(recRoot.get().rawBits(), /*suppress=*/true);
        rejectWithPending(promise);
        return promise.get();
    }
    Rooted<Value> state{Value::fromObject(ObjectHeader::createWithInternalSlots(
        rtHeap(), rtArena(), rtPlainObjectShape(), AsyncFromSyncSlot::kCount))};
    state.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    state.get().asObject<ObjectHeader>()->setInternalSlot(AsyncFromSyncSlot::Promise,
                                                          promise.get());
    state.get().asObject<ObjectHeader>()->setInternalSlot(AsyncFromSyncSlot::Record,
                                                          recRoot.get());
    Rooted<Value> onFulfilled{rtMakeNativeClosure(asyncFromSyncOnValue, state, 1)};
    Rooted<Value> onRejected{rtMakeNativeClosure(asyncFromSyncOnRejected, state, 1)};
    Rooted<Value> noCapability{Value::fromUndefined()};
    rtPerformPromiseThen(wrapper, onFulfilled, onRejected, noCapability);
    return promise.get();
}

}  // namespace

extern "C" {

uint64_t bronze_iter_open(uint64_t srcBits) {
    recordHelperCall("bronze_iter_open");
    Rooted<Value> rec{rtOpenIterator(Value(srcBits))};
    if (rec.get().isObject() &&
        rec.get().asObject<HeapObjectHeader>()->flags == IterRecordHeader::kFlags &&
        (rec.get().asObject<IterRecordHeader>()->kindOf() == IterRecordHeader::Array ||
         rec.get().asObject<IterRecordHeader>()->kindOf() == IterRecordHeader::MapEntries)) {
        const bronze_tls_block* tls = bronze_tls_block_addr();
        if (tls->alloc_limit - tls->alloc_cursor < BRONZE_ABI_ITER_RECORD_BYTES) {
            rtHeap().refill_inline_lab();
        }
    }
    return rec.get().rawBits();
}

bool bronze_iter_step(uint64_t recBits) {
    recordHelperCall("bronze_iter_step");
    Value recVal(recBits);
    if (!recVal.isObject() ||
        recVal.asObject<HeapObjectHeader>()->flags != IterRecordHeader::kFlags) {
        fatal("internal: iter.step on a value that is not an iteration record");
    }
    Rooted<Value> recRoot{recVal};
    auto* rec = recRoot.get().asObject<IterRecordHeader>();
    if (rec->done.asBool()) return false;

    const uint32_t kind = rec->kindOf();

    if (kind == IterRecordHeader::String) {
        StringHeader* str = rec->target.asString<StringHeader>();
        const uint32_t i = rec->cursorOf();
        const uint32_t len = str->getLength();
        if (i >= len) {
            rec->done = Value::fromBool(true);
            rec->current = Value::fromUndefined();
            return false;
        }
        const uint16_t unit = str->charCodeAt(i);
        const bool pair = i + 1 < len && isSurrogatePair(unit, str->charCodeAt(i + 1));
        Value piece;
        if (pair) {
            const uint16_t units[2] = {unit, str->charCodeAt(i + 1)};
            piece = Value::fromString(StringHeader::createUTF16(rtHeap(), units, 2));
        } else if (unit < 0x100) {
            const char byte = static_cast<char>(unit);
            piece = Value::fromString(StringHeader::createLatin1(rtHeap(), &byte, 1));
        } else {
            piece = Value::fromString(StringHeader::createUTF16(rtHeap(), &unit, 1));
        }
        rec = recRoot.get().asObject<IterRecordHeader>();
        rec->current = piece;
        rec->cursor = Value::fromDouble(static_cast<double>(i + (pair ? 2 : 1)));
        return true;
    }

    if (kind == IterRecordHeader::MapEntries) {
        const bool stepped = stepFast(rec);
        rec = recRoot.get().asObject<IterRecordHeader>();
        if (!stepped) {
            rec->done = Value::fromBool(true);
            rec->current = Value::fromUndefined();
            return false;
        }
        const uint32_t slot = rec->cursorOf() - 1;
        Rooted<Value> k{rec->target.asObject<MapHeader>()->keyAt(slot)};
        Rooted<Value> v{rec->target.asObject<MapHeader>()->valueAt(slot)};
        Rooted<Value> pair{Value(bronze_create_array(2))};
        auto* arr = pair.get().asObject<ArrayHeader>();
        arr->elementsData()[0] = k.get();
        arr->elementsData()[1] = v.get();
        recRoot.get().asObject<IterRecordHeader>()->current = pair.get();
        return true;
    }

    if (kind == IterRecordHeader::MapIterator || kind == IterRecordHeader::ArrayIterator) {
        Rooted<Value> it{rec->target};
        Value produced = Value::fromUndefined();
        const bool stepped = kind == IterRecordHeader::MapIterator
                                 ? rtMapIteratorStep(it, produced)
                                 : rtArrayIteratorStep(it, produced);
        rec = recRoot.get().asObject<IterRecordHeader>();
        if (!stepped) {
            rec->done = Value::fromBool(true);
            rec->current = Value::fromUndefined();
            return false;
        }
        rec->current = produced;
        return true;
    }

    if (kind != IterRecordHeader::Protocol) {
        const bool stepped = stepFast(rec);
        rec = recRoot.get().asObject<IterRecordHeader>();
        if (stepped) return true;
        rec->done = Value::fromBool(true);
        rec->current = Value::fromUndefined();
        return false;
    }

    Rooted<Value> nextFn{rec->nextFn};
    Rooted<Value> iterObj{rec->target};
    Rooted<Value> result{callMethod(nextFn, iterObj)};
    if (rtExceptionPending()) {
        recRoot.get().asObject<IterRecordHeader>()->done = Value::fromBool(true);
        return false;
    }
    if (!result.get().isObject()) {
        rtThrowTypeError("the iterator result is not an object");
        recRoot.get().asObject<IterRecordHeader>()->done = Value::fromBool(true);
        return false;
    }
    const bool finished = bronze_truthy(namedProp(result.get(), keyDone()).rawBits());
    if (finished || rtExceptionPending()) {
        rec = recRoot.get().asObject<IterRecordHeader>();
        rec->done = Value::fromBool(true);
        rec->current = Value::fromUndefined();
        return false;
    }
    Rooted<Value> produced{namedProp(result.get(), keyValue())};
    if (rtExceptionPending()) {
        recRoot.get().asObject<IterRecordHeader>()->done = Value::fromBool(true);
        return false;
    }
    recRoot.get().asObject<IterRecordHeader>()->current = produced.get();
    return true;
}

uint64_t bronze_iter_value(uint64_t recBits) {
    recordHelperCall("bronze_iter_value");
    Value recVal(recBits);
    if (!recVal.isObject() ||
        recVal.asObject<HeapObjectHeader>()->flags != IterRecordHeader::kFlags) {
        fatal("internal: iter.value on a value that is not an iteration record");
    }
    return recVal.asObject<IterRecordHeader>()->current.rawBits();
}

void bronze_iter_close(uint64_t recBits, bool suppress) {
    recordHelperCall("bronze_iter_close");
    Value recVal(recBits);
    if (!recVal.isObject() ||
        recVal.asObject<HeapObjectHeader>()->flags != IterRecordHeader::kFlags) {
        fatal("internal: iter.close on a value that is not an iteration record");
    }
    Rooted<Value> recRoot{recVal};
    auto* rec = recRoot.get().asObject<IterRecordHeader>();
    if (rec->kindOf() < IterRecordHeader::Protocol || rec->done.asBool()) return;
    rec->done = Value::fromBool(true);

    const bool inFlight = rtExceptionPending();
    Rooted<Value> inFlightValue{inFlight ? Value(rtTls()->exception_cell)
                                         : Value::fromUndefined()};
    if (inFlight) rtClearException();

    Rooted<Value> iterObj{rec->target};
    Rooted<Value> ret{namedProp(iterObj.get(), keyReturn())};
    Rooted<Value> result{Value::fromUndefined()};
    if (isCallable(ret.get())) {
        result.set(callMethod(ret, iterObj));
    } else if (!inFlight) {
        return;
    }
    if (inFlight) {
        rtClearException();
        rtThrow(inFlightValue.get());
        return;
    }
    if (suppress) {
        if (rtExceptionPending()) rtClearException();
        return;
    }
    if (!rtExceptionPending() && !result.get().isObject()) {
        rtThrowTypeError("iterator return() result is not an object");
    }
}

uint64_t bronze_iter_rest(uint64_t recBits) {
    recordHelperCall("bronze_iter_rest");
    Rooted<Value> recRoot{Value(recBits)};
    Rooted<Value> out{Value(bronze_create_array(0))};
    while (bronze_iter_step(recRoot.get().rawBits())) {
        Rooted<Value> elem{Value(bronze_iter_value(recRoot.get().rawBits()))};
        const uint32_t at = out.get().asObject<ArrayHeader>()->length;
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, elem);
        if (rtExceptionPending()) break;
    }
    return out.get().rawBits();
}

uint64_t bronze_async_iter_open(uint64_t srcBits) {
    recordHelperCall("bronze_async_iter_open");
    return rtOpenAsyncIterator(Value(srcBits)).rawBits();
}

uint64_t bronze_async_iter_next(uint64_t recBits) {
    recordHelperCall("bronze_async_iter_next");
    Value recVal(recBits);
    if (!recVal.isObject() ||
        recVal.asObject<HeapObjectHeader>()->flags != IterRecordHeader::kFlags) {
        fatal("internal: async_iter.next on a value that is not an iteration record");
    }
    Rooted<Value> recRoot{recVal};
    auto* rec = recRoot.get().asObject<IterRecordHeader>();
    if (rec->kindOf() == IterRecordHeader::AsyncProtocol) {
        Rooted<Value> nextFn{rec->nextFn};
        Rooted<Value> target{rec->target};
        return callMethod(nextFn, target).rawBits();
    }
    return asyncFromSyncNext(recRoot).rawBits();
}

void bronze_async_iter_close(uint64_t recBits, bool suppress) {
    bronze_iter_close(recBits, suppress);
}

}  // extern "C"

Value asyncIteratorMethodOf(Value v) {
    if (!v.isObject()) return Value::fromUndefined();
    if (v.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) {
        return proxyMethodOf(v, rtAsyncIteratorKey());
    }
    if (!HeapKind::carriesShape(v.asObject<HeapObjectHeader>()->flags)) {
        return Value::fromUndefined();
    }
    Rooted<Value> objRoot{v};
    Rooted<Value> keyRoot{rtAsyncIteratorKey()};
    return objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), keyRoot);
}

Value rtOpenAsyncIterator(Value source) {
    Rooted<Value> srcRoot{source};
    Rooted<Value> asyncMethod{asyncIteratorMethodOf(srcRoot.get())};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (isCallable(asyncMethod.get())) {
        Rooted<Value> iter{callMethod(asyncMethod, srcRoot)};
        if (rtExceptionPending()) return Value::fromUndefined();
        if (!iter.get().isObject()) {
            rtThrowTypeError("the result of Symbol.asyncIterator is not an object");
            return Value::fromUndefined();
        }
        Rooted<Value> next{namedProp(iter.get(), keyNext())};
        if (rtExceptionPending()) return Value::fromUndefined();
        if (!isCallable(next.get())) {
            rtThrowTypeError("the async iterator has no `next` method");
            return Value::fromUndefined();
        }
        Rooted<Value> recRoot{Value::fromObject(
            IterRecordHeader::create(rtHeap(), IterRecordHeader::AsyncProtocol))};
        auto* rec = recRoot.get().asObject<IterRecordHeader>();
        rec->target = iter.get();
        rec->nextFn = next.get();
        return recRoot.get();
    }
    // No @@asyncIterator: the SYNC record, which bronze_async_iter_next steps
    // as CreateAsyncFromSyncIterator's `next` would (asyncFromSyncNext).
    return rtOpenIterator(srcRoot.get());
}

}  // namespace bronze::runtime
