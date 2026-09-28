// Structured-clone primitives (embed_clone.h): the runtime's own layouts read
// and built directly, so a host serializer pays for the bytes it writes and
// not for the program-level API it would otherwise have to call per value.

#include "embed/embed_clone.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_date_internal.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/native_handle.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/property_key.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/weak_ref.h"

namespace bronze::embed::clone {

static_assert(sizeof(InlineCache) == kDefineCacheWords * sizeof(uint64_t),
              "kDefineCacheWords must cover one InlineCache");

Kind classify(Value v) noexcept {
    if (!v.isObject()) return Kind::Primitive;
    auto* hdr = v.asObject<HeapObjectHeader>();
    switch (hdr->flags) {
    case HeapKind::Array: return Kind::Array;
    case HeapKind::Function: return Kind::Function;
    case HeapKind::TypedArray: return Kind::TypedArray;
    case HeapKind::ArrayBuffer: return Kind::ArrayBuffer;
    case HeapKind::DataView: return Kind::DataView;
    case HeapKind::RegExp: return Kind::RegExp;
    case HeapKind::Proxy: return Kind::Proxy;
    case HeapKind::Plain: break;
    default: return Kind::Other;
    }
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->internalSlotCount() == 0) {
        // Error is the one brand bronze keeps on the prototype chain
        // (exception.cpp): an ordinary object inheriting from Error.prototype.
        return runtime::rtIsErrorInstance(v) ? Kind::Error : Kind::Plain;
    }
    if (runtime::rtIsMapKind(v)) return Kind::Map;
    if (runtime::rtIsSetKind(v)) return Kind::Set;
    if (runtime::rtIsDateObject(v)) return Kind::Date;
    if (runtime::rtIsHandle(v)) return Kind::Handle;
    if (runtime::rtIsPromiseObject(v)) return Kind::Promise;
    if (runtime::rtIsWeakMapObject(v) || runtime::rtIsWeakSetObject(v) ||
        runtime::rtIsWeakRefObject(v)) {
        return Kind::Weak;
    }
    if (runtime::rtIsErrorInstance(v)) return Kind::Error;
    return Kind::Other;
}

// ---- strings and keys --------------------------------------------------------

static Chars charsOf(const StringHeader* s) noexcept {
    if (s->isLatin1()) return {s->latin1Data(), s->getLength(), false};
    return {s->utf16Data(), s->getLength(), true};
}

Chars stringChars(Value str) noexcept {
    if (!str.isString()) return {};
    return charsOf(str.asString<StringHeader>());
}

Value makeString(const void* data, uint32_t length, bool utf16) {
    ShadowStackFrame frame;
    StringHeader* s =
        utf16 ? StringHeader::createUTF16(runtime::rtHeap(), static_cast<const uint16_t*>(data), length)
              : StringHeader::createLatin1(runtime::rtHeap(), static_cast<const char*>(data), length);
    return Value::fromString(&s->header);
}

Chars keyChars(Key key) noexcept {
    if (!key) return {};
    return charsOf(static_cast<const StringHeader*>(key));
}

Key internKey(const void* data, uint32_t length, bool utf16) {
    ShadowStackFrame frame;
    Rooted<Value> str{makeString(data, length, utf16)};
    PropertyKey k = runtime::rtInternPropertyKey(str.get());
    return k.string();
}

Value keyValue(Key key) noexcept {
    return runtime::rtKeyAsValue(static_cast<const StringHeader*>(key));
}

// ---- plain objects -----------------------------------------------------------

const void* layoutToken(Value plain) noexcept {
    if (!plain.isObject() || plain.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        return nullptr;
    }
    Shape* shape = plain.asObject<ObjectHeader>()->shape;
    if (!shape || shape->isDictionary()) return nullptr;
    return shape;
}

namespace {

bool integerLike(const StringHeader* s, uint32_t& idx) {
    if (!s->isLatin1()) return false;
    return runtime::rtIsIntegerLikeKey(std::string_view(s->latin1Data(), s->getLength()), idx);
}

struct Entry {
    PropertyKey key;
    uint32_t slot;
    bool enumerable;
    bool accessor;
};

}  // namespace

bool ownDataLayout(Value plain, std::vector<PropSlot>& out) {
    out.clear();
    if (!plain.isObject() || plain.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        return false;
    }
    const Shape* shape = plain.asObject<ObjectHeader>()->shape;
    if (!shape) return true;

    // Insertion order first, exactly as Shape::ownKeysInInsertionOrder walks
    // it, but keeping the slot and the accessor bit the key walk drops.
    std::vector<Entry> inserted;
    if (shape->isDictionary()) {
        for (const DictEntry& e : shape->dict->entries) {
            if (!e.live()) continue;
            inserted.push_back({e.key, e.slot, e.enumerable, e.accessor});
        }
    } else {
        for (const Shape* s = shape; s != nullptr; s = s->parent) {
            if (!s->key.valid()) continue;
            inserted.push_back({s->key, s->slot_index, s->enumerable, s->accessor});
        }
        std::reverse(inserted.begin(), inserted.end());
    }

    // 6.1.7.1: integer-like keys ascending ahead of the rest (rtOwnKeysOrdered).
    std::vector<std::pair<uint32_t, PropSlot>> ints;
    out.reserve(inserted.size());
    for (const Entry& e : inserted) {
        if (!e.enumerable || e.key.isSymbol()) continue;
        if (e.accessor) return false;
        StringHeader* s = e.key.string();
        uint32_t idx = 0;
        if (integerLike(s, idx)) {
            ints.push_back({idx, {s, e.slot}});
        } else {
            out.push_back({s, e.slot});
        }
    }
    if (!ints.empty()) {
        std::sort(ints.begin(), ints.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<PropSlot> ordered;
        ordered.reserve(ints.size() + out.size());
        for (const auto& [idx, p] : ints) ordered.push_back(p);
        ordered.insert(ordered.end(), out.begin(), out.end());
        out.swap(ordered);
    }
    return true;
}

Value slotValue(Value plain, uint32_t slot) noexcept {
    return plain.asObject<ObjectHeader>()->getSlot(slot);
}

Value prototypeOf(Value obj) noexcept {
    if (!obj.isObject()) return Value::fromNull();
    auto* hdr = obj.asObject<HeapObjectHeader>();
    if (!HeapKind::carriesShape(hdr->flags)) return Value::fromNull();
    const Shape* shape = reinterpret_cast<ObjectHeader*>(hdr)->shape;
    if (!shape) return Value::fromNull();
    Value p = shape->prototypeValue();
    return p.isObject() ? p : Value::fromNull();
}

Value defineOwn(Value obj, Key key, Value v, uint64_t* cache) {
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    Rooted<Value> val{v};
    Rooted<Value> keyRoot{keyValue(key)};
    ObjectHeader* live = self.get().asObject<ObjectHeader>()->setProp(
        runtime::rtHeap(), runtime::rtArena(), keyRoot, val,
        cache ? runtime::rtAsCache(cache) : nullptr,
        /*enumerable=*/true, /*defineOwn=*/true);
    return Value::fromObject(live);
}

// ---- arrays ------------------------------------------------------------------

uint32_t arrayLength(Value arr) noexcept { return arr.asObject<ArrayHeader>()->length; }

Value arrayElement(Value arr, uint32_t index) noexcept {
    return arr.asObject<ArrayHeader>()->getElem(index);
}

Value newArray(uint32_t length) {
    ShadowStackFrame frame;
    return Value(bronze_create_array(length));
}

Value arrayPut(Value arr, uint32_t index, Value v) {
    ShadowStackFrame frame;
    Rooted<Value> self{arr};
    Rooted<Value> val{v};
    self.get().asObject<ArrayHeader>()->setElem(runtime::rtHeap(), index, val);
    return self.get();
}

// ---- Map / Set ---------------------------------------------------------------

uint32_t collectionPositions(Value coll) noexcept {
    const auto* m = coll.asObject<MapHeader>();
    return m->entryData() ? m->used() : 0;
}

bool collectionEntry(Value coll, uint32_t index, Value& key, Value& value) noexcept {
    const auto* m = coll.asObject<MapHeader>();
    if (!m->entryData() || index >= m->used() || !m->liveAt(index)) return false;
    key = m->keyAt(index);
    value = runtime::rtIsSetKind(coll) ? key : m->valueAt(index);
    return true;
}

Value newMap() {
    ShadowStackFrame frame;
    return runtime::rtNewMap();
}

Value newSet() {
    ShadowStackFrame frame;
    return runtime::rtNewSet();
}

Value collectionPut(Value coll, Value key, Value value) {
    ShadowStackFrame frame;
    Rooted<Value> self{coll};
    Rooted<Value> k{key};
    Rooted<Value> v{runtime::rtIsSetKind(coll) ? key : value};
    MapHeader::set(runtime::rtHeap(), self, k, v);
    return self.get();
}

// ---- Date --------------------------------------------------------------------

double dateValue(Value date) noexcept {
    double t = 0;
    if (!runtime::rtIsDateObject(date)) return t;
    return date.asObject<ObjectHeader>()->internalSlot(runtime::DateSlot::TimeValue).asNumber();
}

Value newDate(double t) {
    ShadowStackFrame frame;
    return runtime::rtMakeDateObject(t);
}

}  // namespace bronze::embed::clone
