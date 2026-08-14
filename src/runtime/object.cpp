#include "runtime/object.h"

#include "runtime/accessor.h"
#include "runtime/fatal.h"

namespace bronze {

// Starts at 1 so that a zero-initialized IC entry — which is what the table
// in a freshly loaded object file is — can never read as "filled at the
// current epoch". A depth > 0 entry with a null shape would miss on the
// shape compare anyway; this makes it miss twice over rather than rely on
// that one guard staying first.
static uint64_t g_protoMutationEpoch = 1;

uint64_t protoMutationEpoch() noexcept { return g_protoMutationEpoch; }
void bumpProtoMutationEpoch() noexcept { ++g_protoMutationEpoch; }

namespace {

// The payload of an object with no internal slots: the fields after the heap
// header, plus the inline property slots. Anything past it is an internal slot,
// which is how `internalSlotCount` recovers the number from `header.size`
// without a field of its own — a field would be one more word on every `{}`
// literal in a program to describe a thing almost none of them have.
constexpr size_t kObjectBasePayload =
    (sizeof(ObjectHeader) - sizeof(HeapObjectHeader)) + ObjectHeader::kInlineSlots * sizeof(Value);

}  // namespace

ObjectHeader* ObjectHeader::create(Heap& heap, NonMovingArena& arena, Shape* shape) {
    return createWithInternalSlots(heap, arena, shape, 0);
}

ObjectHeader* ObjectHeader::createWithInternalSlots(Heap& heap, NonMovingArena& arena,
                                                    Shape* shape, uint32_t count) {
    (void)arena;
    if (!shape) {
        fatal("object creation without a shape (the shape carries the prototype)");
    }
    size_t payload_bytes = kObjectBasePayload + count * sizeof(Value);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* obj = reinterpret_cast<ObjectHeader*>(raw_hdr);
    obj->shape = shape;
    obj->overflow = Value::fromUndefined();

    // The internal slots are initialized with the inline ones and by the same
    // loop: the collector scans every payload word as a Value, so leaving one
    // uninitialized would hand it a pointer made of whatever the semispace last
    // held there.
    Value* slots = obj->slotsData();
    for (uint32_t i = 0; i < kInlineSlots + count; ++i) {
        slots[i] = Value::fromUndefined();
    }
    return obj;
}

uint32_t ObjectHeader::internalSlotCount() const noexcept {
    const size_t payload = header.size - sizeof(HeapObjectHeader);
    if (payload <= kObjectBasePayload) return 0;
    return static_cast<uint32_t>((payload - kObjectBasePayload) / sizeof(Value));
}

Value ObjectHeader::internalSlot(uint32_t index) const {
    if (index >= internalSlotCount()) {
        fatal("internal slot index beyond what the object was created with");
    }
    return slotsData()[kInlineSlots + index];
}

void ObjectHeader::setInternalSlot(uint32_t index, Value val) {
    if (index >= internalSlotCount()) {
        fatal("internal slot index beyond what the object was created with");
    }
    slotsData()[kInlineSlots + index] = val;
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
        if (hdr->flags != HeapKind::Plain) return nullptr;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
    }
    return cur;
}

// A cached hit whose holder is an ANCESTOR is only sound while every object
// between the receiver and that holder is the one the entry was filled against,
// with the slot numbering it had then. Adding a property to a prototype never
// renumbers an existing slot, so the transition tree keeps half of that promise
// for free — but a delete reuses freed slots for unrelated names and a
// prototype swap replaces the holder outright, and both leave a dictionary
// behind. One pointer load per link rules both out, on the proto-hit path only.
ObjectHeader* ObjectHeader::cachedProtoHolder(uint32_t depth, bool& crossedDictionary) noexcept {
    crossedDictionary = false;
    ObjectHeader* cur = this;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!cur->shape) return nullptr;
        Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return nullptr;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return nullptr;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
        if (cur->shape && cur->shape->isDictionary()) {
            crossedDictionary = true;
            return nullptr;
        }
    }
    return cur;
}

void ObjectHeader::setPrototype(NonMovingArena& arena, Rooted<Value>& self, Shape* newRoot) {
    // The most direct form of "the chain an entry was filled against is not
    // the chain any more". Dictionary mode below already makes every walk
    // crossing this object miss; the bump says so in the mechanism that owns
    // the question rather than leaving it to a side effect.
    bumpProtoMutationEpoch();
    toDictionary(arena, self);
    // Only the root of a transition tree carries a prototype, and a dictionary
    // shape belongs to exactly one object — so repointing its root moves this
    // object's prototype and nobody else's.
    self.get().asObject<ObjectHeader>()->shape->root = newRoot;
}

Value ObjectHeader::getProp(Heap& heap, Rooted<Value>& key, InlineCache* ic,
                            const Value* receiver) {
    (void)heap;
    const PropertyKey prop_name = PropertyKey::fromValue(key.get());
    if (!prop_name.valid()) {
        fatal("property key must be a string or a symbol");
    }

    // A symbol key never reaches an inline cache, and that is a fact about the
    // COMPILER rather than a rule enforced here: an entry belongs to a property
    // site whose key is a compile-time constant, and the only syntax that can
    // produce a symbol key is a computed access, which has no site entry. So a
    // key-representation change cannot desynchronise the open-coded fast path
    // in codegen-llvm — it compares a shape pointer and nothing else, and a
    // shape that gained a symbol-keyed transition is a different pointer.
    if (ic && ic->describes(shape)) {
        if (ic->cached_depth == 0) return getSlot(ic->cached_slot);
        bool crossedDictionary = false;
        ObjectHeader* holder = cachedProtoHolder(ic->cached_depth, crossedDictionary);
        if (holder) return holder->getSlot(ic->cached_slot);
        if (!crossedDictionary) {
            // The chain got shorter than the cache says, which the shape check
            // should have caught: the prototype lives on the shape, so it
            // cannot change without the shape changing.
            fatal("inline cache depth outruns the prototype chain (corrupt shape?)");
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
                // and a getter is a call.
                Rooted<Value> self{receiver ? *receiver : Value::fromObject(this)};
                return callGetter(holder->getSlot(info.slot), self);
            }
            // A dictionary receiver's shape is private to one object and its
            // slots are not shape-indexed, so an entry naming it could only
            // ever hit for that object and would go stale on its next delete.
            // The proto-hit case fills only what the HIT path would accept —
            // the walk is asked here rather than the condition restated, so
            // the two cannot drift into disagreeing about the same entry.
            bool crossedDictionary = false;
            if (ic && shape && !shape->isDictionary() &&
                (depth == 0 || cachedProtoHolder(depth, crossedDictionary) == holder)) {
                ic->fill(shape, info.slot, depth);
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
                                    bool defineOwn, const Value* receiver, SetRefusal* refused,
                                    bool writable, bool configurable) {
    const PropertyKey prop_name = PropertyKey::fromValue(key.get());
    if (!prop_name.valid()) {
        fatal("property key must be a string or a symbol");
    }

    // A set-site entry only ever describes an OWN DATA property of a
    // non-dictionary shape (below), so a shape match is a slot write with
    // nothing left to check.
    if (ic && ic->describesOwn(shape)) {
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
            bool noSetter = false;
            callSetter(getSlot(own.slot + 1), recv, val, &noSetter);
            if (noSetter && refused) *refused = SetRefusal::NoSetter;
            return live.get().asObject<ObjectHeader>();
        }
        // A non-writable own property discards the write in sloppy mode
        // (10.1.9.2 -> 10.1.6.3 returns false, and 13.15.2 PutValue only throws
        // for a STRICT reference — the same reading given to a getter-only
        // property). Reachable only through `Object.defineProperty` or
        // `Object.freeze`, both of which put the object in dictionary mode, so
        // the IC fill below is already unreachable for it — but the guard is
        // written here rather than inferred from that, because "the cache
        // happens to miss" is not a reason a write is discarded.
        if (!own.writable) {
            if (refused) *refused = SetRefusal::NotWritable;
            return this;
        }
        if (ic && !shape->isDictionary()) ic->fill(shape, own.slot, /*depth=*/0);
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
            bool noSetter = false;
            callSetter(holder->getSlot(info.slot + 1), recv, val, &noSetter);
            if (noSetter && refused) *refused = SetRefusal::NoSetter;
            return live.get().asObject<ObjectHeader>();
        }
    }

    // A frozen or sealed object adds nothing (10.1.9.2 step 3 -> 10.1.6.3
    // step 2.b), silently, for the same sloppy-mode reason a non-writable
    // property discards its write.
    if (shape->isDictionary() && !shape->dict->extensible) {
        if (refused) *refused = SetRefusal::NotExtensible;
        return this;
    }

    // There is no rule here about what a key is SPELLED like, and there must not
    // be one. A symbol key is enumerable (10.1.9.2 -> 7.3.5 CreateDataProperty:
    // `enumerable: true`) and is absent from `Object.keys` and `for-in` because
    // those are defined over string keys, not because it is hidden — so forcing
    // one non-enumerable would make `Object.assign` and `{ ...o }` drop
    // symbol-keyed properties 10.1.9.2 requires them to copy. And the state the
    // runtime's iterator objects carry is not a property at all now
    // (`ObjectHeader::internalSlot`), so nothing needs a reserved prefix either.

    // Create the own property: a shape transition, or an entry in the
    // dictionary once one delete has made the chain unusable. Both may grow
    // the overflow block, which allocates and can move this object — operate
    // through a root from here on.
    Rooted<Value> self{Value::fromObject(this)};
    uint32_t new_slot = 0;
    ObjectHeader* live = nullptr;
    if (shape->isDictionary()) {
        live = dictDefine(heap, arena, self, prop_name, enumerable, /*accessor=*/false, new_slot);
        DictEntry* entry = self.get().asObject<ObjectHeader>()->shape->dict->find(prop_name);
        if (entry) {
            entry->writable = writable;
            entry->configurable = configurable;
        }
    } else {
        // If this object is somebody's prototype, the property just created
        // shadows whatever the depth > 0 entries below it point at, and their
        // receivers' shapes are untouched — so those entries have to miss. One
        // load and a not-taken branch for every other add, which is what keeps
        // `new Point(x, y)` in a loop from invalidating the whole program's
        // proto caches.
        if (shape->used_as_prototype) bumpProtoMutationEpoch();
        Shape* next_shape =
            shape->addProperty(arena, heap, key, new_slot, enumerable, /*is_accessor=*/false,
                               writable, configurable);
        live = ensureSlots(heap, self, new_slot + 1);
        live->shape = next_shape;
        if (ic) ic->fill(next_shape, new_slot, /*depth=*/0);
    }
    live->setSlot(new_slot, val.get());
    return live;
}

}  // namespace bronze
