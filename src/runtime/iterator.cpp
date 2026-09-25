// The iterator protocol: %IteratorPrototype%, %AsyncIteratorPrototype%,
// the per-kind prototypes and shape initialization, and result object creation.

#include "runtime/iterator.h"
#include "runtime/iterator_internal.h"

#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/async_generator.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/generator.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/symbol.h"

namespace bronze::runtime {

bool isCallable(Value v) {
    return rtIsCallableValue(v);
}

Value callMethod(Rooted<Value>& fn, Rooted<Value>& thisValue) {
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisValue.get().rawBits(), 0, nullptr));
}

Value namedProp(Value obj, StringHeader* key) {
    if (!obj.isObject()) return Value::fromUndefined();
    const uint16_t kind = obj.asObject<HeapObjectHeader>()->flags;
    if (kind == ProxyHeader::kFlags) {
        Rooted<Value> objRoot{obj};
        Rooted<Value> keyRoot{rtKeyAsValue(key)};
        return rtProxyGet(objRoot.get(), keyRoot.get(), objRoot.get());
    }
    if (!HeapKind::carriesShape(kind)) return Value::fromUndefined();
    Rooted<Value> objRoot{obj};
    Rooted<Value> keyRoot{Value::fromString(key)};
    return objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), keyRoot);
}

Value proxyMethodOf(Value proxy, Value symbolKey) {
    Rooted<Value> objRoot{proxy};
    Rooted<Value> keyRoot{symbolKey};
    Rooted<Value> method{rtProxyGet(objRoot.get(), keyRoot.get(), objRoot.get())};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (method.get().isNull() || method.get().isUndefined()) return Value::fromUndefined();
    return method.get();
}

static StringHeader* internKey(const char* text) {
    return StringHeader::createLatin1InArena(rtArena(), text, static_cast<uint32_t>(std::strlen(text)));
}

StringHeader* keyNext() {
    static thread_local StringHeader* k = internKey("next");
    return k;
}
StringHeader* keyDone() {
    static thread_local StringHeader* k = internKey("done");
    return k;
}
StringHeader* keyValue() {
    static thread_local StringHeader* k = internKey("value");
    return k;
}
StringHeader* keyReturn() {
    static thread_local StringHeader* k = internKey("return");
    return k;
}

Value rtIteratorKey() { return Value::fromSymbol(rtSymbolIterator()); }
Value rtAsyncIteratorKey() { return Value::fromSymbol(rtSymbolAsyncIterator()); }

uint64_t iteratorProtoSelf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return thisBits;
}

namespace {

uint64_t asyncIteratorProtoSelf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    recordHelperCall("asyncIteratorProtoSelf");
    return thisBits;
}

struct ProtoEntry {
    Value proto = Value::fromUndefined();
    Shape* shape = nullptr;
};

ProtoEntry& protoEntry(IteratorProto kind) {
    static thread_local ProtoEntry table[9];
    return table[static_cast<uint32_t>(kind)];
}

Value iteratorPrototypeRoot() {
    static thread_local Value root = Value::fromUndefined();
    if (root.isObject()) return root;
    Rooted<Value> obj{
        Value::fromObject(ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    Rooted<Value> key{rtIteratorKey()};
    Rooted<Value> self{rtNativeFunction(iteratorProtoSelf, 0, "[Symbol.iterator]", 0)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, self);
    rtInstallIteratorHelpers(obj);
    rtInstallIteratorPrototypeAccessors(obj);
    root = obj.get();
    rtHeap().add_permanent_root(&root);
    return root;
}

Value asyncIteratorPrototypeRoot() {
    static thread_local Value root = Value::fromUndefined();
    if (root.isObject()) return root;
    Rooted<Value> obj{
        Value::fromObject(ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape()))};
    Rooted<Value> key{rtAsyncIteratorKey()};
    Rooted<Value> self{rtNativeFunction(asyncIteratorProtoSelf, 0, "[Symbol.asyncIterator]", 0)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, self);
    root = obj.get();
    rtHeap().add_permanent_root(&root);
    return root;
}

constexpr uint32_t kInternalSlots[] = {
    MapIteratorSlot::kCount,
    MapIteratorSlot::kCount,
    ArrayIteratorSlot::kCount,
    RegExpStringIteratorSlot::kCount,
    GeneratorSlot::kCount,
    StringIteratorSlot::kCount,
    AsyncGeneratorSlot::kCount,
    IteratorHelperSlot::kCount,
    IteratorHelperSlot::kCount,
};

Shape* iteratorObjectShape(IteratorProto kind) {
    ProtoEntry& entry = protoEntry(kind);
    if (entry.shape) return entry.shape;
    Rooted<Value> parent{kind == IteratorProto::AsyncGenerator ? asyncIteratorPrototypeRoot()
                                                              : iteratorPrototypeRoot()};
    Rooted<Value> proto{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(parent.get())))};
    entry.proto = proto.get();
    rtHeap().add_permanent_root(&entry.proto);
    if (kind == IteratorProto::Generator) {
        rtInstallGeneratorPrototype(proto);
        entry.proto = proto.get();
    } else if (kind == IteratorProto::AsyncGenerator) {
        rtInstallAsyncGeneratorPrototype(proto);
        entry.proto = proto.get();
    } else if (kind == IteratorProto::Helper) {
        rtInstallIteratorHelperPrototype(proto);
        entry.proto = proto.get();
    } else if (kind == IteratorProto::Wrap) {
        rtInstallIteratorWrapPrototype(proto);
        entry.proto = proto.get();
    }
    switch (kind) {
        case IteratorProto::Map:
            rtDefineToStringTag(proto, "Map Iterator");
            break;
        case IteratorProto::Set:
            rtDefineToStringTag(proto, "Set Iterator");
            break;
        case IteratorProto::Array:
            rtDefineToStringTag(proto, "Array Iterator");
            break;
        case IteratorProto::RegExpString:
            rtDefineToStringTag(proto, "RegExp String Iterator");
            break;
        case IteratorProto::Generator:
            rtDefineToStringTag(proto, "Generator");
            break;
        case IteratorProto::String:
            rtDefineToStringTag(proto, "String Iterator");
            break;
        case IteratorProto::AsyncGenerator:
            rtDefineToStringTag(proto, "AsyncGenerator");
            break;
        case IteratorProto::Helper:
            rtDefineToStringTag(proto, "Iterator Helper");
            break;
        case IteratorProto::Wrap:
            break;
    }
    entry.proto = proto.get();
    entry.shape = rtNewRootShape(entry.proto);
    return entry.shape;
}

}  // namespace

Value rtCreateIterResult(Rooted<Value>& value, bool done) {
    static thread_local Shape* shape = nullptr;
    if (shape == nullptr) {
        uint32_t slot = 0;
        Shape* s = rtPlainObjectShape()->addPropertyKey(
            rtArena(), PropertyKey::forString(keyValue()), slot, /*is_enumerable=*/true,
            /*is_accessor=*/false, /*is_writable=*/true, /*is_configurable=*/true,
            SlotRepr::Boxed);
        s = s->addPropertyKey(rtArena(), PropertyKey::forString(keyDone()), slot,
                              /*is_enumerable=*/true, /*is_accessor=*/false,
                              /*is_writable=*/true, /*is_configurable=*/true, SlotRepr::Boxed);
        shape = s;
    }
    ObjectHeader* obj = ObjectHeader::create(rtHeap(), rtArena(), shape);
    obj->header.flags = HeapKind::Plain;
    static_assert(ObjectHeader::kInlineSlots >= 2, "a result's two slots are inline");
    obj->setSlot(0, value.get());
    obj->setSlot(1, Value::fromBool(done));
    return Value::fromObject(obj);
}

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
    const Shape* root = obj->shape ? obj->shape->root : nullptr;
    return root && root->prototype.rawBits() == rtIteratorPrototype(kind).rawBits();
}

Value rtIteratorPrototype(IteratorProto kind) {
    iteratorObjectShape(kind);
    return protoEntry(kind).proto;
}

Value rtIteratorSharedPrototype() { return iteratorPrototypeRoot(); }

}  // namespace bronze::runtime
