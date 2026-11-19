// Object spread and object rest between plain objects.
//
// The general walk (rt_spread.cpp) builds the source's ordered key list, makes
// a heap string of each name, reads it back through a property lookup and
// defines it on the target through another. None of that is needed when the
// source is a plain object on the transition tree with nothing but data
// properties: its shape chain IS the key list, in insertion order, and each
// node says which slot the value is in. So the names are read off the chain
// and the values out of the slots.
//
// The write side is where the rest of the time went, one transition lookup
// and one slot-block check per key. A program copies the same few layouts
// over and over (`{ ...defaults, ...options }`), and what a copy does to the
// target's layout is a function of three things: the source's shape, the
// target's shape before, and which of the source's keys are copied. So the
// first copy of a kind is done a key at a time and what it came to is
// remembered: the shape the target ended at and, per key, the slot read and
// the slot written. The next one stores the shape, sizes the slot block once
// and moves the values. Shapes are immutable and immortal, so an entry stays
// true for as long as the arena does (`rtForgetSpreadShapeHints`).
//
// Object.assign is the same copy with each key Set instead of defined, and the
// two writes differ only where the target's prototype chain answers the key
// (a setter, a read-only property, a proxy) or the target owns it with other
// attributes. A copy where neither happens is a spread, and its hint also
// records the prototype epoch, which every change to a chain moves.

#include "runtime/spread_plain.h"

#include <cstdint>

#include "runtime/array.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_property.h"
#include "runtime/realm.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

// More own properties than this and the copy takes the general walk: the
// nodes are gathered on the stack.
constexpr uint32_t kMaxKeys = 48;

// Every property of the source: a spread's `kept`. A rest pattern's is one
// bit per property it keeps, counted from the newest; it can never be this,
// which would be more keys than a copy gathers.
constexpr uint64_t kAllKept = ~0ull;

struct ShapeHint {
    const Shape* source;
    const Shape* before;
    Shape* after;
    uint64_t kept;
    // An assign's hint: the chain answered none of the keys at this epoch.
    uint64_t epoch;
    bool assign;
    // Every copied property sits inline with compatible slot representation,
    // so copyAsBefore skips ensureSlots() and setSlot() generalization checks.
    bool directInlineCopy;
    uint16_t count;
    // Slots an object at `after` needs (ObjectHeader::ensureSlots).
    uint16_t slotCount;
    uint16_t from[kMaxKeys];
    uint16_t to[kMaxKeys];
};
constexpr uint32_t kHintCount = 128;
// Plain data, so thread teardown has nothing to run for it.
thread_local ShapeHint t_hints[kHintCount];

// A rest pattern's excluded keys, read out of their array once: the walk
// below asks about every property of the source.
constexpr uint32_t kMaxExcluded = 16;

struct RestHint {
    const Shape* source;
    const Shape* before;
    uint64_t epoch;
    bool assign;
    uint8_t excludedCount;
    const void* excludedIds[kMaxExcluded];
    ShapeHint hint;
};
constexpr uint32_t kRestHintCount = 64;
thread_local RestHint t_restHints[kRestHintCount];

ShapeHint& hintFor(const Shape* source, const Shape* before, uint64_t kept, bool assign) noexcept {
    const uint64_t a = reinterpret_cast<uintptr_t>(source);
    const uint64_t b = reinterpret_cast<uintptr_t>(before);
    const uint64_t h = (a ^ (b * 0x9E3779B97F4A7C15ull) ^ kept ^ static_cast<uint64_t>(assign)) * 0xD6E8FEB86659FD93ull;
    return t_hints[(h >> 40) % kHintCount];
}

bool hintMatches(const ShapeHint& hint, const Shape* source, const Shape* before, uint64_t kept,
                 bool assign) noexcept {
    return hint.source == source && hint.before == before && hint.kept == kept &&
           hint.assign == assign && (!assign || hint.epoch == protoMutationEpoch());
}

uint64_t restKeysHash(const void* const* ids, uint32_t count) noexcept {
    uint64_t h = 0x517cc1b727220a95ull ^ count;
    for (uint32_t i = 0; i < count; ++i) {
        h = (h * 0x9E3779B97F4A7C15ull) ^ reinterpret_cast<uintptr_t>(ids[i]);
    }
    return h;
}

RestHint& restHintFor(const Shape* source, const Shape* before, uint64_t keysHash,
                      uint32_t excludedCount, bool assign) noexcept {
    const uint64_t a = reinterpret_cast<uintptr_t>(source);
    const uint64_t b = reinterpret_cast<uintptr_t>(before);
    const uint64_t h =
        (a ^ (b * 0x9E3779B97F4A7C15ull) ^ keysHash ^
         (static_cast<uint64_t>(excludedCount) << 8) ^ static_cast<uint64_t>(assign)) *
        0xD6E8FEB86659FD93ull;
    return t_restHints[(h >> 40) % kRestHintCount];
}

bool restHintMatches(const RestHint& rh, const Shape* source, const Shape* before,
                     const void* const* ids, uint32_t count, bool assign) noexcept {
    if (rh.source != source || rh.before != before || rh.assign != assign ||
        rh.excludedCount != count) {
        return false;
    }
    if (assign && rh.epoch != protoMutationEpoch()) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (rh.excludedIds[i] != ids[i]) return false;
    }
    return true;
}

bool canDirectInlineCopy(const Shape* source, const Shape* after, const ShapeHint& hint) noexcept {
    if (hint.slotCount > ObjectHeader::kOverflowSlot) return false;
    for (uint32_t i = 0; i < hint.count; ++i) {
        if (hint.from[i] >= ObjectHeader::kOverflowSlot || hint.to[i] >= ObjectHeader::kOverflowSlot) {
            return false;
        }
        if (after->slotIsDouble(hint.to[i]) && !source->slotIsDouble(hint.from[i])) return false;
        if (after->slotIsObject(hint.to[i]) && !source->slotIsObject(hint.from[i])) return false;
    }
    return true;
}

void storeRestHint(const Shape* source, const Shape* before, uint64_t restHash,
                   const void* const* excludedIds, uint32_t excludedLen, bool assign,
                   const ShapeHint& hint) noexcept {
    RestHint& rh = restHintFor(source, before, restHash, excludedLen, assign);
    rh.source = source;
    rh.before = before;
    rh.epoch = assign ? protoMutationEpoch() : 0;
    rh.assign = assign;
    rh.excludedCount = static_cast<uint8_t>(excludedLen);
    for (uint32_t i = 0; i < excludedLen; ++i) rh.excludedIds[i] = excludedIds[i];
    rh.hint = hint;
}

// A plain object with no internal slots, on the transition tree.
ObjectHeader* treePlain(Value v) noexcept {
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return nullptr;
    auto* obj = v.asObject<ObjectHeader>();
    if (!obj->isBarePlain() || obj->shape->isDictionary()) return nullptr;
    return obj;
}

// A name 6.1.7.1 orders ahead of the others (an array index), or one this
// walk cannot tell from one cheaply: every index begins with a digit.
bool mayBeIndex(const StringHeader* s) noexcept {
    if (s->getLength() == 0 || !s->isLatin1()) return false;
    const uint8_t c = static_cast<uint8_t>(s->latin1Data()[0]);
    return c >= '0' && c <= '9';
}

struct Excluded {
    PropertyKey keys[kMaxExcluded];
    uint32_t count = 0;

    bool has(PropertyKey name) const noexcept {
        for (uint32_t i = 0; i < count; ++i) {
            if (keys[i].matches(name)) return true;
        }
        return false;
    }
};

bool plainData(const PropertyInfo& p) noexcept {
    return !p.accessor && p.writable && p.enumerable && p.configurable;
}

// Whether Set of `key` on `target` is CreateDataProperty: the target owns it as
// plain data, or it lacks it and no object on its chain has it as anything but
// writable data. Every link must be a plain object, whose named properties are
// all in its shape; a proxy, an array or a wrapper answers some from elsewhere.
bool setIsDefine(const ObjectHeader* target, PropertyKey key) noexcept {
    PropertyInfo info;
    if (target->shape->lookupProperty(key, info)) return plainData(info);
    const ObjectHeader* holder = target;
    for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        const Value parent = holder->shape->prototypeValue();
        if (parent.isNull()) return true;
        if (!parent.isObject() || parent.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
            return false;
        }
        holder = parent.asObject<ObjectHeader>();
        if (holder->shape == nullptr) return false;
        if (holder->shape->lookupProperty(key, info)) return !info.accessor && info.writable;
    }
    return false;
}

// Whether `after` is `before` plus the keys of `nodes` it lacked, each key a
// plain data property of it; `slots` then holds where each key's value goes.
// What makes a finished copy worth remembering: a target that replaced an
// accessor, or moved to a dictionary, did something a slot write does not.
bool isPlainAppend(const Shape* after, const Shape* before, const Shape* const* nodes,
                   uint32_t count, uint16_t* slots, uint32_t& slotCount) noexcept {
    if (after->isDictionary() || after->root != before->root) return false;
    uint32_t added = 0;
    slotCount = 0;
    for (const Shape* s = after; s != before; s = s->parent) {
        if (s == nullptr || !s->key.valid() || ++added > count) return false;
        if (s->slot_index + 1 > slotCount) slotCount = s->slot_index + 1;
    }
    // The keys `before` lacks are as many as were added: then those ARE what
    // was added, and nothing else.
    uint32_t absent = 0;
    const bool empty = !before->key.valid();
    for (uint32_t i = 0; i < count; ++i) {
        PropertyInfo info;
        if (!after->lookupProperty(nodes[i]->key, info) || !plainData(info)) return false;
        if (info.slot > UINT16_MAX || nodes[i]->slot_index > UINT16_MAX) return false;
        slots[i] = static_cast<uint16_t>(info.slot);
        PropertyInfo own;
        if (empty || !before->lookupProperty(nodes[i]->key, own)) ++absent;
    }
    return absent == added && slotCount <= UINT16_MAX;
}

// The copy a remembered one stands for. The one allocation comes ahead of
// every raw pointer.
void copyAsBefore(const ShapeHint& hint, Rooted<Value>& target, Rooted<Value>& src) {
    if (hint.directInlineCopy) {
        ObjectHeader* to = target.get().asObject<ObjectHeader>();
        const ObjectHeader* from = src.get().asObject<ObjectHeader>();
        to->shape = hint.after;
        const Value* srcSlots = from->slotsData();
        if (rtHeap().gc().is_young(reinterpret_cast<uintptr_t>(to))) {
            auto* dstSlots = reinterpret_cast<Value*>(to->slotsData());
            for (uint32_t i = 0; i < hint.count; ++i) {
                dstSlots[hint.to[i]] = srcSlots[hint.from[i]];
            }
        } else {
            HeapValue* dstSlots = to->slotsData();
            for (uint32_t i = 0; i < hint.count; ++i) {
                dstSlots[hint.to[i]] = srcSlots[hint.from[i]];
            }
        }
        return;
    }
    ObjectHeader* to = ObjectHeader::ensureSlots(rtHeap(), target, hint.slotCount);
    const ObjectHeader* from = src.get().asObject<ObjectHeader>();
    to->shape = hint.after;
    // `setSlot`, not a raw store: a slot the shape holds as a double takes a
    // number as one and anything else by generalizing, which moves the object
    // to a sibling shape with the same slots.
    for (uint32_t i = 0; i < hint.count; ++i) to->setSlot(hint.to[i], from->rawSlot(hint.from[i]));
}

}  // namespace

void rtForgetSpreadShapeHints() noexcept {
    for (ShapeHint& h : t_hints) h = ShapeHint{};
    for (RestHint& r : t_restHints) r = RestHint{};
}

bool rtSpreadPlain(Rooted<Value>& target, Rooted<Value>& src, const Rooted<Value>* excluded,
                   bool assign) {
    const ObjectHeader* from = treePlain(src.get());
    if (from == nullptr) return false;
    // A plain object off the dictionary is extensible (integrity.h keeps the
    // levels in the dictionary). A prototype's adds have to be seen one at a
    // time by the caches below it, and a global's by the global cache.
    const ObjectHeader* to = treePlain(target.get());
    if (to == nullptr || to->shape->used_as_prototype) return false;
    if (assign && rtRealmOfGlobal(target.get()) != nullptr) return false;
    const Shape* const source = from->shape;
    const Shape* const before = to->shape;

    if (!excluded) {
        const ShapeHint& hint = hintFor(source, before, kAllKept, assign);
        if (hintMatches(hint, source, before, kAllKept, assign)) {
            copyAsBefore(hint, target, src);
            return true;
        }
    }

    // The source's properties off the chain, newest first; the ones to copy.
    // Nothing allocates before the walk is over, so the excluded keys can be
    // held raw.
    if (source->has_symbol_keys) return false;
    Excluded skip;
    const void* excludedIds[kMaxExcluded] = {};
    uint32_t excludedLen = 0;
    uint64_t restHash = 0;
    bool allImmortalExcluded = false;
    if (excluded) {
        const auto* keys = excluded->get().asObject<ArrayHeader>();
        excludedLen = keys->length;
        if (excludedLen > kMaxExcluded) return false;
        allImmortalExcluded = true;
        for (uint32_t i = 0; i < excludedLen; ++i) {
            const PropertyKey pk = PropertyKey::fromValue(keys->getElem(i));
            skip.keys[skip.count++] = pk;
            if (!pk.valid() || !pk.immortal()) {
                allImmortalExcluded = false;
            } else {
                excludedIds[i] = pk.identity();
            }
        }
        if (allImmortalExcluded) {
            restHash = restKeysHash(excludedIds, excludedLen);
            const RestHint& rh = restHintFor(source, before, restHash, excludedLen, assign);
            if (restHintMatches(rh, source, before, excludedIds, excludedLen, assign)) {
                copyAsBefore(rh.hint, target, src);
                return true;
            }
        }
    }
    const Shape* nodes[kMaxKeys];
    uint32_t count = 0;
    uint32_t position = 0;
    uint64_t kept = 0;
    for (const Shape* s = source; s != nullptr; s = s->parent) {
        if (!s->key.valid()) continue;
        const StringHeader* name = s->key.string();
        // A heap-string key moves; an accessor runs code; an index sorts first.
        if (s->accessor || name == nullptr || !s->key.immortal() || mayBeIndex(name)) return false;
        if (position == 64) return false;
        const uint32_t at = position++;
        if (!s->enumerable || skip.has(s->key)) continue;
        if (count == kMaxKeys) return false;
        nodes[count++] = s;
        kept |= 1ull << at;
    }
    if (count == 0) {
        if (allImmortalExcluded) {
            ShapeHint emptyHint{};
            emptyHint.source = source;
            emptyHint.before = before;
            emptyHint.after = const_cast<Shape*>(before);
            emptyHint.kept = 0;
            emptyHint.assign = assign;
            emptyHint.directInlineCopy = true;
            storeRestHint(source, before, restHash, excludedIds, excludedLen, assign, emptyHint);
        }
        return true;
    }
    if (!excluded) kept = kAllKept;
    for (uint32_t i = 0, j = count - 1; i < j; ++i, --j) {
        const Shape* t = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = t;
    }

    const ShapeHint& hitHint = hintFor(source, before, kept, assign);
    if (hintMatches(hitHint, source, before, kept, assign)) {
        copyAsBefore(hitHint, target, src);
        if (allImmortalExcluded) {
            storeRestHint(source, before, restHash, excludedIds, excludedLen, assign, hitHint);
        }
        return true;
    }
    if (assign) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!setIsDefine(to, nodes[i]->key)) return false;
        }
    }

    // The first copy of its kind: define the keys one at a time. Nothing here
    // runs user code, so the source's shape and the nodes gathered from it
    // stay what they were.
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> val{src.get().asObject<ObjectHeader>()->rawSlot(nodes[i]->slot_index)};
        if (!rtCreateDataPropertyOrThrow(target, nodes[i]->key, val, /*ic=*/nullptr)) return true;
    }
    const ObjectHeader* done = treePlain(target.get());
    uint32_t slotCount = 0;
    ShapeHint next{};
    if (done != nullptr && isPlainAppend(done->shape, before, nodes, count, next.to, slotCount)) {
        next.source = source;
        next.before = before;
        next.after = done->shape;
        next.kept = kept;
        next.assign = assign;
        next.epoch = assign ? protoMutationEpoch() : 0;
        next.count = static_cast<uint16_t>(count);
        next.slotCount = static_cast<uint16_t>(slotCount);
        for (uint32_t i = 0; i < count; ++i) next.from[i] = static_cast<uint16_t>(nodes[i]->slot_index);
        next.directInlineCopy = canDirectInlineCopy(source, done->shape, next);
        hintFor(source, before, kept, assign) = next;
        if (allImmortalExcluded) {
            storeRestHint(source, before, restHash, excludedIds, excludedLen, assign, next);
        }
    }
    return true;
}

}  // namespace bronze::runtime
