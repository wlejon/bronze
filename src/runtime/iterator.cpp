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

#include "runtime/generator.h"
#include "runtime/iterator.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"

namespace bronze {

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

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// A named property of a PLAIN object, by its arena-interned key. Every object
// this asks about is one the protocol built — an iterator, or the `{ value,
// done }` record `next` returned — so anything else answers `undefined` and
// the caller's own check reports it.
Value namedProp(Value obj, StringHeader* key) {
    if (!obj.isObject() ||
        obj.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        return Value::fromUndefined();
    }
    Rooted<Value> objRoot{obj};
    Rooted<Value> keyRoot{Value::fromString(key)};
    return objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), keyRoot);
}

StringHeader* internKey(const char* text) {
    StringHeader* tmp = StringHeader::createFromUTF8(rtHeap(), std::string_view(text));
    return StringHeader::internToArena(rtArena(), tmp);
}

StringHeader* keyNext() {
    static StringHeader* k = internKey("next");
    return k;
}
StringHeader* keyDone() {
    static StringHeader* k = internKey("done");
    return k;
}
StringHeader* keyValue() {
    static StringHeader* k = internKey("value");
    return k;
}
StringHeader* keyReturn() {
    static StringHeader* k = internKey("return");
    return k;
}

// The `[Symbol.iterator]` method of a value, or `undefined`. Only a plain
// object can carry one: an array, a string and a typed array take the fast
// kinds above, and a Map answers for itself in `openRecord`.
//
// The key is a SYMBOL, so this walks the prototype chain looking for a symbol
// key and can no longer be confused by a string property that happens to spell
// the hook's old name.
Value iteratorMethodOf(Value v) {
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        return Value::fromUndefined();
    }
    Rooted<Value> objRoot{v};
    Rooted<Value> keyRoot{rtIteratorKey()};
    return objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), keyRoot);
}

// Everything 7.4.2 GetIterator does, into an already-created record.
// `recRoot` holds the record; `srcRoot` the value being iterated.
void openProtocol(Rooted<Value>& recRoot, Rooted<Value>& srcRoot) {
    Rooted<Value> method{iteratorMethodOf(srcRoot.get())};
    if (!isCallable(method.get())) {
        rtThrowTypeError(rtIterableKindName(srcRoot.get()) + " is not iterable");
        return;
    }
    Rooted<Value> iter{method.get().asObject<FunctionHeader>()->call(srcRoot.get(), 0, nullptr)};
    if (rtExceptionPending()) return;
    if (!iter.get().isObject()) {
        rtThrowTypeError("the result of Symbol.iterator is not an object");
        return;
    }
    Rooted<Value> next{namedProp(iter.get(), keyNext())};
    if (!isCallable(next.get())) {
        rtThrowTypeError("the iterator has no `next` method");
        return;
    }
    auto* rec = recRoot.get().asObject<IterRecordHeader>();
    rec->kind = Value::fromDouble(static_cast<double>(IterRecordHeader::Protocol));
    rec->target = iter.get();
    rec->nextFn = next.get();
}

bool stepFast(IterRecordHeader* rec) {
    const uint32_t i = rec->cursorOf();
    switch (rec->kindOf()) {
        case IterRecordHeader::Array: {
            auto* arr = rec->target.asObject<ArrayHeader>();
            if (i >= arr->length) return false;
            // 23.1.5.1 reads with Get, so a HOLE iterates as `undefined` rather
            // than being skipped.
            rec->current = arr->getElem(i);
            rec->cursor = Value::fromDouble(static_cast<double>(i + 1));
            return true;
        }
        case IterRecordHeader::TypedArray: {
            auto* view = rec->target.asObject<TypedArrayHeader>();
            if (i >= view->length) return false;
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

Value rtIteratorKey() { return Value::fromSymbol(rtSymbolIterator()); }

namespace {

// %IteratorPrototype%'s one member (27.1.2.1): an iterator is its own iterable,
// which is what lets `for (const x of m.keys())` and `[...gen()]` work — the
// for-of opens the value it is given, and the value it is given is already an
// iterator.
uint64_t iteratorProtoSelf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return thisBits;
}

// The five per-kind prototypes plus the %IteratorPrototype% they share, in one
// table indexed by `IteratorProto`. Permanent roots: the collector moves plain
// objects, and a `static Value` it has not been told about would be an address
// recorded before a collection and read after one.
struct ProtoEntry {
    Value proto = Value::fromUndefined();
    Shape* shape = nullptr;
};

ProtoEntry& protoEntry(IteratorProto kind) {
    static ProtoEntry table[6];
    return table[static_cast<uint32_t>(kind)];
}

// %IteratorPrototype% (27.1.2). Inherits Object.prototype, like every other
// intrinsic object, so `m.entries().hasOwnProperty` is the method a program
// would find in a spec engine rather than `undefined`.
Value iteratorPrototypeRoot() {
    static Value root = Value::fromUndefined();
    if (root.isObject()) return root;
    Rooted<Value> obj{
        Value::fromObject(ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    Rooted<Value> key{rtIteratorKey()};
    Rooted<Value> self{rtNativeFunction(iteratorProtoSelf, 0)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, self);
    root = obj.get();
    rtHeap().add_permanent_root(&root);
    return root;
}

// How many internal slots each kind's objects are allocated with, in the order
// `IteratorProto` declares. Half the brand, so it is written once and read by
// both the creator and the check.
constexpr uint32_t kInternalSlots[] = {
    MapIteratorSlot::kCount,
    MapIteratorSlot::kCount,
    ArrayIteratorSlot::kCount,
    RegExpStringIteratorSlot::kCount,
    GeneratorSlot::kCount,
    StringIteratorSlot::kCount,
};

Shape* iteratorObjectShape(IteratorProto kind) {
    ProtoEntry& entry = protoEntry(kind);
    if (entry.shape) return entry.shape;
    // The kind's own prototype. For all but the generator's it has NO members
    // of its own: bronze puts their `next` on the iterator itself, which is a
    // divergence recorded in cases/collection_internal_slots.js and not one
    // this seam decides. %GeneratorPrototype% is the exception, and not by
    // choice — 27.5.1 defines `next`, `return` and `throw` there, and a
    // generator object with an own `next` would report one where a spec engine
    // reports none.
    Rooted<Value> parent{iteratorPrototypeRoot()};
    Rooted<Value> proto{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(parent.get())))};
    entry.proto = proto.get();
    rtHeap().add_permanent_root(&entry.proto);
    if (kind == IteratorProto::Generator) {
        rtInstallGeneratorPrototype(proto);
        // Re-read through the permanent root: installing the methods allocates,
        // so the address `proto` was built at may already be stale.
        entry.proto = proto.get();
    }
    // `@@toStringTag`, which is what `Object.prototype.toString` reads (20.1.3.6
    // step 15) and the only reason these objects have any own property beyond
    // the generator's three methods.
    switch (kind) {
        case IteratorProto::Map:
            rtDefineToStringTag(proto, "Map Iterator");  // 24.1.5.2.2
            break;
        case IteratorProto::Set:
            rtDefineToStringTag(proto, "Set Iterator");  // 24.2.5.2.2
            break;
        case IteratorProto::Array:
            // 23.1.5.2.2 — and a typed array's iterator shares this prototype
            // by 23.2.5.2, so the one object really is the one ECMA-262 has.
            rtDefineToStringTag(proto, "Array Iterator");
            break;
        case IteratorProto::RegExpString:
            rtDefineToStringTag(proto, "RegExp String Iterator");  // 22.2.9.1.2
            break;
        case IteratorProto::Generator:
            rtDefineToStringTag(proto, "Generator");  // 27.5.1.5
            break;
        case IteratorProto::String:
            rtDefineToStringTag(proto, "String Iterator");  // 22.1.5.1.2
            break;
    }
    // Defining the tag allocates, for the same reason installing the generator
    // methods above does.
    entry.proto = proto.get();
    // The root shape holds the prototype, and `rtNewRootShape` hands it to the
    // collector — so the object above is reachable twice over, once for the
    // shape and once for the static this function reads it back from.
    entry.shape = rtNewRootShape(entry.proto);
    return entry.shape;
}

}  // namespace

Value rtNewIteratorObject(IteratorProto kind) {
    const uint32_t slots = kInternalSlots[static_cast<uint32_t>(kind)];
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(),
                                                              iteratorObjectShape(kind), slots);
    obj->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    return Value::fromObject(obj);
}

bool rtIsIteratorObject(Value v, IteratorProto kind) {
    if (!v.isObject()) return false;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->internalSlotCount() != kInternalSlots[static_cast<uint32_t>(kind)]) return false;
    // The prototype lives on the shape's ROOT, which a delete carries across
    // into dictionary mode — so an iterator a program has deleted a property
    // from is still branded. `Object.setPrototypeOf` is the one thing that
    // unbrands one, and that is a program saying it is no longer that object.
    const Shape* root = obj->shape ? obj->shape->root : nullptr;
    return root && root->prototype.rawBits() == rtIteratorPrototype(kind).rawBits();
}

Value rtIteratorPrototype(IteratorProto kind) {
    iteratorObjectShape(kind);
    return protoEntry(kind).proto;
}

// The kind of a value, for the "is not iterable" TypeError. Its own function
// because `rt_object.cpp`'s copy answers the same question for "is not a
// function" and neither wants the other's spelling of `undefined`.
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
        // Neither is iterable, and both are the mistake a program makes when it
        // means the view over the buffer rather than the bytes themselves.
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
            case TypedArrayHeader::kFlags: kind = IterRecordHeader::TypedArray; break;
            case MapHeader::kMapFlags: kind = IterRecordHeader::MapEntries; break;
            case MapHeader::kSetFlags: kind = IterRecordHeader::SetValues; break;
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

}  // namespace bronze::runtime

namespace bronze::runtime {

extern "C" {

uint64_t bronze_iter_open(uint64_t srcBits) {
    recordHelperCall("bronze_iter_open");
    return rtOpenIterator(Value(srcBits)).rawBits();
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

    // A string steps by CODE POINT: a surrogate pair is one iteration
    // yielding a two-unit string, which is why the cursor is not an `i + 1`
    // anywhere (kept).
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
        // Re-derived: creating the piece allocates, so the record may have
        // moved out from under the pointer taken above.
        rec = recRoot.get().asObject<IterRecordHeader>();
        rec->current = piece;
        rec->cursor = Value::fromDouble(static_cast<double>(i + (pair ? 2 : 1)));
        return true;
    }

    if (kind == IterRecordHeader::MapEntries &&
        rec->target.asObject<HeapObjectHeader>()->flags == MapHeader::kMapFlags) {
        // A Map's default iterator yields [key, value] pairs (24.1.3.12), so
        // this is the one fast kind that allocates per element.
        if (!stepFast(rec)) {
            rec->done = Value::fromBool(true);
            rec->current = Value::fromUndefined();
            return false;
        }
        const uint32_t slot = rec->cursorOf() - 1;
        Rooted<Value> k{rec->target.asObject<MapHeader>()->keyAt(slot)};
        Rooted<Value> v{rec->target.asObject<MapHeader>()->valueAt(slot)};
        Rooted<Value> pair{Value(bronze_create_array(2))};
        pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, k);
        pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, v);
        recRoot.get().asObject<IterRecordHeader>()->current = pair.get();
        return true;
    }

    if (kind != IterRecordHeader::Protocol) {
        if (stepFast(rec)) return true;
        rec->done = Value::fromBool(true);
        rec->current = Value::fromUndefined();
        return false;
    }

    // The protocol: one call into user code per element (7.4.6 IteratorStep).
    Rooted<Value> nextFn{rec->nextFn};
    Rooted<Value> iterObj{rec->target};
    Rooted<Value> result{nextFn.get().asObject<FunctionHeader>()->call(iterObj.get(), 0, nullptr)};
    if (rtExceptionPending()) {
        // Nothing more to close: 7.4.6 leaves an iterator whose `next` threw
        // for the caller's throw completion to carry, and calling `return` on
        // it afterwards is exactly what 7.4.9 does not do.
        recRoot.get().asObject<IterRecordHeader>()->done = Value::fromBool(true);
        return false;
    }
    if (!result.get().isObject()) {
        rtThrowTypeError("the iterator result is not an object");
        recRoot.get().asObject<IterRecordHeader>()->done = Value::fromBool(true);
        return false;
    }
    if (bronze_truthy(namedProp(result.get(), keyDone()).rawBits())) {
        rec = recRoot.get().asObject<IterRecordHeader>();
        rec->done = Value::fromBool(true);
        rec->current = Value::fromUndefined();
        return false;
    }
    Rooted<Value> produced{namedProp(result.get(), keyValue())};
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

// IteratorClose (7.4.9). A fast kind has nothing to close; a protocol
// iterator that is already exhausted has nothing to close either, because
// 7.4.9 is only reached for an iteration abandoned before `done`.
//
// `suppress` is step 6: when a throw is already on its way out, an error the
// `return` method raises is DISCARDED rather than replacing it. The caller
// that passes true has already taken the pending value with `exc.take`, so
// "already on its way out" is not something this can see for itself.
void bronze_iter_close(uint64_t recBits, bool suppress) {
    recordHelperCall("bronze_iter_close");
    Value recVal(recBits);
    if (!recVal.isObject() ||
        recVal.asObject<HeapObjectHeader>()->flags != IterRecordHeader::kFlags) {
        fatal("internal: iter.close on a value that is not an iteration record");
    }
    Rooted<Value> recRoot{recVal};
    auto* rec = recRoot.get().asObject<IterRecordHeader>();
    if (rec->kindOf() != IterRecordHeader::Protocol || rec->done.asBool()) return;
    rec->done = Value::fromBool(true);

    Rooted<Value> iterObj{rec->target};
    Rooted<Value> ret{namedProp(iterObj.get(), keyReturn())};
    // 7.4.9 step 4: an iterator with no `return` closes by doing nothing.
    if (!isCallable(ret.get())) return;
    ret.get().asObject<FunctionHeader>()->call(iterObj.get(), 0, nullptr);
    if (suppress && rtExceptionPending()) rtClearException();
}

// A rest element's value: everything the cursor has left, as a fresh array.
// Drains the same record the elements before it were stepped from, which is
// what makes `const [a,...rest] = someSet` see the elements after `a` rather
// than restarting the iteration.
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

}  // extern "C"

}  // namespace bronze::runtime
