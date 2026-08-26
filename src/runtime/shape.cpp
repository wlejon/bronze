#include "runtime/shape.h"

#include <algorithm>

#include <vector>

#include "runtime/fatal.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/slot_repr.h"

namespace bronze {

Shape* Shape::createRoot(NonMovingArena& arena, Value proto) {
    Shape* root = arena.create<Shape>();
    root->prototype = proto;
    // This is the ONE moment an object becomes a prototype — every route to one
    // (a class, `Object.create`, `Object.setPrototypeOf`) ends in a root shape
    // carrying it — so it is where the mark is applied rather than at each of
    // those callers.
    //
    // Only a plain object is marked: `cachedProtoHolder` refuses to walk
    // through anything else, so an array or a function used as a prototype
    // already misses for a reason that has nothing to do with the epoch.
    if (proto.isObject()) {
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
            auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
            if (Shape* protoShape = obj->shape) {
                Shape* plainShape = runtime::rtCurrentPlainObjectShape();
                if (plainShape && protoShape == plainShape) {
                    Shape* dedicated = arena.create<Shape>();
                    dedicated->prototype = protoShape->prototype;
                    dedicated->used_as_prototype = true;
                    obj->shape = dedicated;
                    runtime::rtRegisterRootShape(dedicated);
                } else {
                    protoShape->used_as_prototype = true;
                }
            }
        }
    }
    return root;
}

Shape* Shape::addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name,
                          uint32_t& out_slot, bool is_enumerable, bool is_accessor,
                          bool is_writable, bool is_configurable, SlotRepr desired) {
    (void)heap;
    PropertyKey incoming = PropertyKey::fromValue(name.get());
    if (!incoming.valid()) {
        fatal("property name must be a string or a symbol");
    }
    // Shapes are immortal and non-moving, so they must never point into the
    // movable heap. A STRING key is copied into the arena, which is sound
    // exactly because a string key is matched by content — the copy is the
    // same key. A SYMBOL is already in the arena and must NOT be copied: a
    // symbol is matched by identity, so a copy would be a different key
    // that nothing could ever look up again (runtime/symbol.h).
    //
    // Done BEFORE the transition scan and not only on the create path, so the
    // key handed to `addPropertyKey` is always an arena key — which is what
    // lets that entry be the one place a node is minted.
    PropertyKey stored =
        incoming.isSymbol()
            ? incoming
            : PropertyKey::forString(StringHeader::internToArena(arena, incoming.string()));
    return addPropertyKey(arena, stored, out_slot, is_enumerable, is_accessor, is_writable,
                          is_configurable, desired);
}

Shape* Shape::addPropertyKey(NonMovingArena& arena, PropertyKey stored, uint32_t& out_slot,
                             bool is_enumerable, bool is_accessor, bool is_writable,
                             bool is_configurable, SlotRepr desired) {
    if (!stored.valid()) {
        fatal("property name must be a string or a symbol");
    }
    if (isDictionary()) {
        fatal("internal: a shape transition attempted on a dictionary-mode object");
    }
    // An accessor pair is two slots holding two function references; there is
    // no double representation to give it, and the width arithmetic below
    // assumes none exists.
    if (is_accessor) desired = SlotRepr::Boxed;

    // Two passes, because the representation is part of the key and the edge
    // this store WANTS may not be the one it gets. A double edge that has
    // already generalized once is a demotion this key has earned, and taking it
    // again would split the tree a second time for a field that has proven it
    // turns over — so the sticky bit sends the store to the boxed edge instead.
    Shape* next_shape = nullptr;
    Shape* boxed_edge = nullptr;
    for (const auto& trans : transitions) {
        if (trans.enumerable != is_enumerable || trans.accessor != is_accessor ||
            trans.writable != is_writable || trans.configurable != is_configurable ||
            !trans.key.matches(stored)) {
            continue;
        }
        if (trans.repr == SlotRepr::Boxed) boxed_edge = trans.next_shape;
        if (trans.repr != desired) continue;
        next_shape = trans.next_shape;
    }
    if (next_shape != nullptr && desired == SlotRepr::Double && next_shape->repr_generalized) {
        desired = SlotRepr::Boxed;
        next_shape = boxed_edge;
    }
    if (next_shape != nullptr) {
        out_slot = next_shape->slot_index;
    } else {
        uint32_t next_slot = nextSlotIndex();

        if (next_slot + (is_accessor ? 1u : 0u) >= kDictionaryThreshold) {
            fatal("object has too many properties for the shape transition tree; "
                  "dictionary mode is not implemented");
        }
        if (next_slot >= kSlotReprLimit) desired = SlotRepr::Boxed;

        out_slot = next_slot;
        next_shape =
            arena.create<Shape>(this, stored, next_slot, root, is_enumerable, is_accessor,
                                is_writable, is_configurable, desired);
        transitions.push_back(ShapeTransition{stored, next_shape, is_enumerable, is_accessor,
                                              is_writable, is_configurable, desired});
        if (desired == SlotRepr::Double) {
            ++runtime::slotReprMutableCounters().double_nodes;
        } else {
            ++runtime::slotReprMutableCounters().boxed_nodes;
        }
    }

    // An object that was a prototype before this add is still one after it, so
    // the mark has to reach the shape it lands on or the NEXT add would not
    // bump the epoch. On the reuse path as much as the create path: a shape is
    // marked when some object first becomes a prototype, which can be long
    // after its transitions were built by unrelated objects.
    if (used_as_prototype) next_shape->used_as_prototype = true;
    return next_shape;
}

Shape* Shape::withSlotBoxed(NonMovingArena& arena, Shape* shape, uint32_t index) {
    if (shape == nullptr || shape->isDictionary() || !shape->slotIsDouble(index)) return shape;

    // Root-first order, because a transition tree is only walkable downward
    // from its root and the chain links the other way.
    std::vector<Shape*> chain;
    for (Shape* cur = shape; cur != nullptr && cur->key.valid(); cur = cur->parent) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());

    Shape* rebuilt = shape->root;
    for (Shape* node : chain) {
        SlotRepr repr = node->repr;
        if (node->slot_index == index && repr == SlotRepr::Double) {
            // The demotion, and the sticky mark that keeps the next object to
            // install this key from taking the same edge back.
            if (!node->repr_generalized) {
                node->repr_generalized = true;
                ++runtime::slotReprMutableCounters().generalized_nodes;
            }
            repr = SlotRepr::Boxed;
        }
        uint32_t slot = 0;
        rebuilt = rebuilt->addPropertyKey(arena, node->key, slot, node->enumerable, node->accessor,
                                          node->writable, node->configurable, repr);
        if (slot != node->slot_index) {
            // The rebuild walked the same names with the same widths in the
            // same order, so it must have handed out the same numbers. If it
            // did not, an object is about to be told its properties moved.
            fatal("internal: slot renumbered while generalizing a double slot");
        }
    }
    if (shape->used_as_prototype) rebuilt->used_as_prototype = true;
    ++runtime::slotReprMutableCounters().generalizations;
    return rebuilt;
}

std::vector<PropertyKey> Shape::ownKeysInInsertionOrder(bool enumerableOnly) const {
    std::vector<PropertyKey> keys;
    if (isDictionary()) {
        for (const DictEntry& e : dict->entries) {
            if (enumerableOnly && !e.enumerable) continue;
            keys.push_back(e.key);
        }
        return keys;
    }
    for (const Shape* curr = this; curr != nullptr; curr = curr->parent) {
        if (!curr->key.valid()) continue;
        if (enumerableOnly && !curr->enumerable) continue;
        keys.push_back(curr->key);
    }
    // Collected newest-first walking toward the root; insertion order is
    // the reverse.
    std::reverse(keys.begin(), keys.end());
    return keys;
}

bool Shape::lookupProperty(PropertyKey name, PropertyInfo& out) const noexcept {
    if (!name.valid()) return false;

    if (isDictionary()) {
        const DictEntry* e = dict->find(name);
        if (!e) return false;
        out.slot = e->slot;
        out.enumerable = e->enumerable;
        out.accessor = e->accessor;
        out.writable = e->writable;
        out.configurable = e->configurable;
        out.repr = SlotRepr::Boxed;
        return true;
    }

    const Shape* curr = this;
    while (curr != nullptr) {
        if (curr->key.matches(name)) {
            out.slot = curr->slot_index;
            out.enumerable = curr->enumerable;
            out.accessor = curr->accessor;
            out.writable = curr->writable;
            out.configurable = curr->configurable;
            out.repr = curr->repr;
            return true;
        }
        curr = curr->parent;
    }
    return false;
}

bool Shape::lookupProperty(PropertyKey name, uint32_t& out_slot) const noexcept {
    PropertyInfo info;
    if (!lookupProperty(name, info)) return false;
    out_slot = info.slot;
    return true;
}

}  // namespace bronze
