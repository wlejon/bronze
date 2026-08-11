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
                          uint32_t& out_slot, bool is_enumerable) {
    (void)heap;
    if (!name.get().isString()) {
        fatal("property name must be a string");
    }

    StringHeader* prop_str = name.get().asString<StringHeader>();

    for (const auto& trans : transitions) {
        if (trans.enumerable == is_enumerable && trans.property_name &&
            trans.property_name->equals(*prop_str)) {
            out_slot = trans.next_shape->slot_index;
            return trans.next_shape;
        }
    }

    // A root shape owns no property, so the first property added to it
    // takes slot 0; every other shape owns the property at slot_index, so
    // the next one goes after it.
    const bool isRoot = (property_name == nullptr);
    uint32_t next_slot = isRoot ? 0 : slot_index + 1;

    if (next_slot >= kDictionaryThreshold) {
        fatal("object has too many properties for the shape transition tree; "
              "dictionary mode is not implemented");
    }

    out_slot = next_slot;
    // Shapes are immortal and non-moving, so they must never point into
    // the movable heap: property names are copied into the arena.
    StringHeader* interned = StringHeader::internToArena(arena, prop_str);
    Shape* next_shape = arena.create<Shape>(this, interned, next_slot, root, is_enumerable);
    transitions.push_back(ShapeTransition{interned, next_shape, is_enumerable});
    return next_shape;
}

std::vector<StringHeader*> Shape::ownKeysInInsertionOrder(bool enumerableOnly) const {
    std::vector<StringHeader*> keys;
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

bool Shape::lookupProperty(StringHeader* name, uint32_t& out_slot) const noexcept {
    if (!name) return false;

    const Shape* curr = this;
    while (curr != nullptr) {
        if (curr->property_name && curr->property_name->equals(*name)) {
            out_slot = curr->slot_index;
            return true;
        }
        curr = curr->parent;
    }
    return false;
}

}  // namespace bronze
