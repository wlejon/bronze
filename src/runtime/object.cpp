#include "runtime/object.h"

#include "runtime/accessor.h"
#include "runtime/fatal.h"
#include "runtime/iterator.h"

namespace bronze {

ObjectHeader* ObjectHeader::create(Heap& heap, NonMovingArena& arena, Shape* shape) {
    (void)arena;
    if (!shape) {
        fatal("object creation without a shape (the shape carries the prototype)");
    }
    size_t payload_bytes =
        (sizeof(ObjectHeader) - sizeof(HeapObjectHeader)) + kInlineSlots * sizeof(Value);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* obj = reinterpret_cast<ObjectHeader*>(raw_hdr);
    obj->shape = shape;
    obj->overflow = Value::fromUndefined();

    Value* slots = obj->slotsData();
    for (uint32_t i = 0; i < kInlineSlots; ++i) {
        slots[i] = Value::fromUndefined();
    }
    return obj;
}

Value ObjectHeader::getSlot(uint32_t index) const {
    if (index < kInlineSlots) {
        return slotsData()[index];
    }
    uint32_t oi = index - kInlineSlots;
    if (oi >= overflowCapacity()) {
        fatal("object slot index beyond overflow capacity (corrupt shape?)");
    }
    return overflow.asObject<HeapObjectHeader>()->payload<Value>()[oi];
}

void ObjectHeader::setSlot(uint32_t index, Value val) {
    if (index < kInlineSlots) {
        slotsData()[index] = val;
        return;
    }
    uint32_t oi = index - kInlineSlots;
    if (oi >= overflowCapacity()) {
        fatal("object slot index beyond overflow capacity (corrupt shape?)");
    }
    overflow.asObject<HeapObjectHeader>()->payload<Value>()[oi] = val;
}

// Grow self's overflow block to hold at least `needed` out-of-line slots.
// Allocation may collect and move both the object and its old block, so
// everything is re-derived through the root after allocating.
ObjectHeader* ObjectHeader::ensureOverflow(Heap& heap, Rooted<Value>& self, uint32_t needed) {
    auto* obj = self.get().asObject<ObjectHeader>();
    uint32_t cap = obj->overflowCapacity();
    if (needed <= cap) {
        return obj;
    }
    uint32_t new_cap = cap ? cap * 2 : ObjectHeader::kInlineSlots;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    HeapObjectHeader* block = heap.allocate(new_cap * sizeof(Value), Tag::Object);
    obj = self.get().asObject<ObjectHeader>();

    Value* slots = block->payload<Value>();
    uint32_t i = 0;
    if (obj->overflow.isPointer()) {
        Value* old_slots = obj->overflow.asObject<HeapObjectHeader>()->payload<Value>();
        for (; i < cap; ++i) {
            slots[i] = old_slots[i];
        }
    }
    for (; i < new_cap; ++i) {
        slots[i] = Value::fromUndefined();
    }
    obj->overflow = Value::fromObject(block);
    return obj;
}

// Slot indices [0, count) must be addressable. An accessor occupies two, so
// the caller's "highest slot" and "slot count" differ by more than one and
// the arithmetic belongs here rather than at each call site.
ObjectHeader* ObjectHeader::ensureSlots(Heap& heap, Rooted<Value>& self, uint32_t count) {
    if (count <= kInlineSlots) return self.get().asObject<ObjectHeader>();
    return ensureOverflow(heap, self, count - kInlineSlots);
}

ObjectHeader* ObjectHeader::protoAncestor(uint32_t depth) noexcept {
    ObjectHeader* cur = this;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!cur->shape) return nullptr;
        Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return nullptr;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags != 0) return nullptr;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
    }
    return cur;
}

// A cached hit whose holder is an ANCESTOR is only sound while that
// ancestor's slot numbering is the one the entry was filled against. Adding
// a property to a prototype never renumbers an existing slot, so the
// transition tree keeps that promise for free — but a delete does not, and
// a dictionary reuses freed slots for unrelated names (docs/0019 decision
// 5). One pointer load rules that out, on the proto-hit path only.
static bool cachedProtoHolderIsStale(const ObjectHeader* holder) noexcept {
    return holder->shape != nullptr && holder->shape->isDictionary();
}

Value ObjectHeader::getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic,
                            const Value* receiver) {
    (void)heap;
    if (!key.get().isString()) {
        fatal("property key must be a string");
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    if (ic && ic->cached_shape == shape) {
        ObjectHeader* holder = protoAncestor(ic->cached_depth);
        if (!holder) {
            // The chain got shorter than the cache says, which the shape check
            // should have caught: the prototype lives on the shape, so it
            // cannot change without the shape changing.
            fatal("inline cache depth outruns the prototype chain (corrupt shape?)");
        }
        if (ic->cached_depth == 0 || !cachedProtoHolderIsStale(holder)) {
            return holder->getSlot(ic->cached_slot);
        }
        // Fall through and look it up properly; the entry is refilled below,
        // or left alone if the answer is no longer cacheable.
    }

    // Own property first, then up the prototype chain. Nothing here
    // allocates until an accessor is found, so these raw pointers stay valid
    // for the whole walk — and the accessor branch stops using them.
    ObjectHeader* holder = this;
    for (uint32_t depth = 0; depth <= kMaxPrototypeDepth; ++depth) {
        PropertyInfo info;
        if (holder->shape && holder->shape->lookupProperty(prop_name, info)) {
            if (info.accessor) {
                // Deliberately NOT cached: every consumer of an entry reads it
                // as a slot index, including the load generated code inlines,
                // and a getter is a call (docs/0019 decision 5).
                Rooted<Value> self{receiver ? *receiver : Value::fromObject(this)};
                return callGetter(holder->getSlot(info.slot), self);
            }
            // A dictionary receiver's shape is private to one object and its
            // slots are not shape-indexed, so an entry naming it could only
            // ever hit for that object and would go stale on its next delete.
            if (ic && shape && !shape->isDictionary() &&
                !(depth > 0 && cachedProtoHolderIsStale(holder))) {
                ic->cached_shape = shape;
                ic->cached_slot = info.slot;
                ic->cached_depth = depth;
            }
            return holder->getSlot(info.slot);
        }
        ObjectHeader* next = holder->protoAncestor(1);
        if (!next) {
            return Value::fromUndefined();
        }
        holder = next;
    }
    fatal("prototype chain too deep (a cycle?)");
}

ObjectHeader* ObjectHeader::setProp(Heap& heap, NonMovingArena& arena, Rooted<Value>& key,
                                    Rooted<Value>& val, InlineCache* ic, bool enumerable,
                                    bool defineOwn, const Value* receiver) {
    if (!key.get().isString()) {
        fatal("property key must be a string");
    }
    StringHeader* prop_name = key.get().asString<StringHeader>();

    // A set-site entry only ever describes an OWN DATA property of a
    // non-dictionary shape (below), so a shape match is a slot write with
    // nothing left to check.
    if (ic && ic->cached_shape == shape && ic->cached_depth == 0) {
        setSlot(ic->cached_slot, val.get());
        return this;
    }

    PropertyInfo own;
    if (shape && shape->lookupProperty(prop_name, own)) {
        if (own.accessor) {
            if (defineOwn) {
                // CreateDataProperty over an accessor would have to strip the
                // pair and hand the name a data slot at the same position.
                // Dictionary mode can express that; nothing asks for it yet,
                // so it is named rather than half-built.
                fatal("redefining an accessor property as a data property is unsupported");
            }
            Rooted<Value> live{Value::fromObject(this)};
            Rooted<Value> recv{receiver ? *receiver : live.get()};
            callSetter(getSlot(own.slot + 1), recv, val);
            return live.get().asObject<ObjectHeader>();
        }
        // A non-writable own property discards the write in sloppy mode
        // (10.1.9.2 -> 10.1.6.3 returns false, and 13.15.2 PutValue only
        // throws for a STRICT reference — the same reading docs/0019
        // decision 6 gives a getter-only property). Reachable only through
        // `Object.defineProperty` or `Object.freeze`, both of which put the
        // object in dictionary mode, so the IC fill below is already
        // unreachable for it — but the guard is written here rather than
        // inferred from that, because "the cache happens to miss" is not a
        // reason a write is discarded.
        if (!own.writable) return this;
        if (ic && !shape->isDictionary()) {
            ic->cached_shape = shape;
            ic->cached_slot = own.slot;
            ic->cached_depth = 0;
        }
        setSlot(own.slot, val.get());
        return this;
    }

    // No own property. An assignment then walks the prototype chain looking
    // for an ACCESSOR to run (ECMA-262 10.1.9.2): an inherited setter takes
    // the write, an inherited data property is merely shadowed by the new own
    // one. Before accessors existed this walk could be skipped outright,
    // which is why the write path used to say it never walked.
    if (!defineOwn) {
        ObjectHeader* holder = this;
        for (uint32_t depth = 1; depth <= kMaxPrototypeDepth; ++depth) {
            holder = holder->protoAncestor(1);
            if (!holder) break;
            PropertyInfo info;
            if (!holder->shape || !holder->shape->lookupProperty(prop_name, info)) continue;
            if (!info.accessor) break;  // shadowed by the own property created below
            Rooted<Value> live{Value::fromObject(this)};
            Rooted<Value> recv{receiver ? *receiver : live.get()};
            callSetter(holder->getSlot(info.slot + 1), recv, val);
            return live.get().asObject<ObjectHeader>();
        }
    }

    // A frozen or sealed object adds nothing (10.1.9.2 step 3 -> 10.1.6.3
    // step 2.b), silently, for the same sloppy-mode reason a non-writable
    // property discards its write.
    if (shape->isDictionary() && !shape->dict->extensible) return this;

    // A well-known symbol key is not an enumerable property. bronze spells
    // `Symbol.iterator` as the string `"@@iterator"` (docs/0021 decision 1),
    // and a real symbol key would never appear in `Object.keys`, `for-in`,
    // object spread or console.log — so the string standing in for one must
    // not either. The rule is on the KEY rather than on the call site
    // because there are four call sites and one of them is `o[k] = v` with a
    // computed key, where nothing but the key is known.
    if (runtime::rtIsWellKnownSymbolKey(prop_name)) enumerable = false;

    // Create the own property: a shape transition, or an entry in the
    // dictionary once one delete has made the chain unusable. Both may grow
    // the overflow block, which allocates and can move this object — operate
    // through a root from here on.
    Rooted<Value> self{Value::fromObject(this)};
    uint32_t new_slot = 0;
    ObjectHeader* live = nullptr;
    if (shape->isDictionary()) {
        live = dictDefine(heap, arena, self, prop_name, enumerable, /*accessor=*/false, new_slot);
    } else {
        Shape* next_shape = shape->addProperty(arena, heap, key, new_slot, enumerable);
        live = ensureSlots(heap, self, new_slot + 1);
        live->shape = next_shape;
        if (ic) {
            ic->cached_shape = next_shape;
            ic->cached_slot = new_slot;
            ic->cached_depth = 0;
        }
    }
    live->setSlot(new_slot, val.get());
    return live;
}

}  // namespace bronze
