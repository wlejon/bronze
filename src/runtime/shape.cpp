#include "runtime/shape.h"

#include <algorithm>

#include "runtime/fatal.h"

namespace bronze {

Shape* Shape::createRoot(NonMovingArena& arena, Value proto) {
    Shape* root = arena.create<Shape>();
    root->prototype = proto;
    return root;
}

Shape* Shape::addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name,
                          uint32_t& out_slot, bool is_enumerable, bool is_accessor) {
    (void)heap;
    if (!name.get().isString()) {
        fatal("property name must be a string");
    }
    if (isDictionary()) {
        fatal("internal: a shape transition attempted on a dictionary-mode object");
    }

    StringHeader* prop_str = name.get().asString<StringHeader>();

    for (const auto& trans : transitions) {
        if (trans.enumerable == is_enumerable && trans.accessor == is_accessor &&
            trans.property_name && trans.property_name->equals(*prop_str)) {
            out_slot = trans.next_shape->slot_index;
            return trans.next_shape;
        }
    }

    uint32_t next_slot = nextSlotIndex();

    if (next_slot + (is_accessor ? 1u : 0u) >= kDictionaryThreshold) {
        fatal("object has too many properties for the shape transition tree; "
              "dictionary mode is not implemented");
    }

    out_slot = next_slot;
    // Shapes are immortal and non-moving, so they must never point into
    // the movable heap: property names are copied into the arena.
    StringHeader* interned = StringHeader::internToArena(arena, prop_str);
    Shape* next_shape =
        arena.create<Shape>(this, interned, next_slot, root, is_enumerable, is_accessor);
    transitions.push_back(ShapeTransition{interned, next_shape, is_enumerable, is_accessor});
    return next_shape;
}

std::vector<StringHeader*> Shape::ownKeysInInsertionOrder(bool enumerableOnly) const {
    std::vector<StringHeader*> keys;
    if (isDictionary()) {
        for (const DictEntry& e : dict->entries) {
            if (enumerableOnly && !e.enumerable) continue;
            keys.push_back(e.name);
        }
        return keys;
    }
    for (const Shape* curr = this; curr != nullptr; curr = curr->parent) {
        if (!curr->property_name) continue;
        if (enumerableOnly && !curr->enumerable) continue;
        keys.push_back(curr->property_name);
    }
    // Collected newest-first walking toward the root; insertion order is
    // the reverse.
    std::reverse(keys.begin(), keys.end());
    return keys;
}

bool Shape::lookupProperty(StringHeader* name, PropertyInfo& out) const noexcept {
    if (!name) return false;

    if (isDictionary()) {
        const DictEntry* e = dict->find(name);
        if (!e) return false;
        out.slot = e->slot;
        out.enumerable = e->enumerable;
        out.accessor = e->accessor;
        return true;
    }

    const Shape* curr = this;
    while (curr != nullptr) {
        if (curr->property_name && curr->property_name->equals(*name)) {
            out.slot = curr->slot_index;
            out.enumerable = curr->enumerable;
            out.accessor = curr->accessor;
            return true;
        }
        curr = curr->parent;
    }
    return false;
}

bool Shape::lookupProperty(StringHeader* name, uint32_t& out_slot) const noexcept {
    PropertyInfo info;
    if (!lookupProperty(name, info)) return false;
    out_slot = info.slot;
    return true;
}

}  // namespace bronze
