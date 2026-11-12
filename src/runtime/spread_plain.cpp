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

#include "runtime/spread_plain.h"

#include <cstdint>

#include "runtime/array.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_property.h"
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
    uint16_t count;
    // Slots an object at `after` needs (ObjectHeader::ensureSlots).
    uint16_t slotCount;
    uint16_t from[kMaxKeys];
    uint16_t to[kMaxKeys];
};
constexpr uint32_t kHintCount = 64;
// Plain data, so thread teardown has nothing to run for it.
thread_local ShapeHint t_hints[kHintCount];

ShapeHint& hintFor(const Shape* source, const Shape* before, uint64_t kept) noexcept {
    const uint64_t a = reinterpret_cast<uintptr_t>(source);
    const uint64_t b = reinterpret_cast<uintptr_t>(before);
    const uint64_t h = (a ^ (b * 0x9E3779B97F4A7C15ull) ^ kept) * 0xD6E8FEB86659FD93ull;
    return t_hints[(h >> 40) % kHintCount];
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

// A rest pattern's excluded keys, read out of their array once: the walk
// below asks about every property of the source.
constexpr uint32_t kMaxExcluded = 16;
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
}

bool rtSpreadPlain(Rooted<Value>& target, Rooted<Value>& src, const Rooted<Value>* excluded) {
    const ObjectHeader* from = treePlain(src.get());
    if (from == nullptr) return false;
    // A plain object off the dictionary is extensible (integrity.h keeps the
    // levels in the dictionary). A prototype's adds have to be seen one at a
    // time by the caches below it.
    const ObjectHeader* to = treePlain(target.get());
    if (to == nullptr || to->shape->used_as_prototype) return false;
    const Shape* const source = from->shape;
    const Shape* const before = to->shape;

    if (!excluded) {
        const ShapeHint& hint = hintFor(source, before, kAllKept);
        if (hint.source == source && hint.before == before && hint.kept == kAllKept) {
            copyAsBefore(hint, target, src);
            return true;
        }
    }

    // The source's properties off the chain, newest first; the ones to copy.
    // Nothing allocates before the walk is over, so the excluded keys can be
    // held raw.
    if (source->has_symbol_keys) return false;
    Excluded skip;
    if (excluded) {
        const auto* keys = excluded->get().asObject<ArrayHeader>();
        if (keys->length > kMaxExcluded) return false;
        for (uint32_t i = 0; i < keys->length; ++i) {
            skip.keys[skip.count++] = PropertyKey::fromValue(keys->getElem(i));
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
    if (count == 0) return true;
    if (!excluded) kept = kAllKept;
    for (uint32_t i = 0, j = count - 1; i < j; ++i, --j) {
        const Shape* t = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = t;
    }

    ShapeHint& hint = hintFor(source, before, kept);
    if (hint.source == source && hint.before == before && hint.kept == kept) {
        copyAsBefore(hint, target, src);
        return true;
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
        next.count = static_cast<uint16_t>(count);
        next.slotCount = static_cast<uint16_t>(slotCount);
        for (uint32_t i = 0; i < count; ++i) next.from[i] = static_cast<uint16_t>(nodes[i]->slot_index);
        hint = next;
    }
    return true;
}

}  // namespace bronze::runtime
