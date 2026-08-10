#include "runtime/shape.h"

#include <stdexcept>

namespace bronze {

Shape* Shape::createRoot(NonMovingArena& arena, Shape* prototype) {
    return arena.create<Shape>(nullptr, nullptr, 0, prototype);
}

Shape* Shape::addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name, uint32_t& out_slot) {
    (void)heap;
    if (!name.get().isString()) {
        throw std::runtime_error("Property name must be a string");
    }

    StringHeader* prop_str = name.get().asString<StringHeader>();

    for (const auto& trans : transitions) {
        if (trans.property_name && trans.property_name->equals(*prop_str)) {
            out_slot = trans.next_shape->slot_index;
            return trans.next_shape;
        }
    }

    uint32_t next_slot = (parent == nullptr && property_name == nullptr && slot_index == 0 && transitions.empty())
                             ? 0
                             : (slot_index + 1);

    // If this is the root shape (slot_index == 0 and property_name == nullptr) and we already have properties or not,
    // let's be careful: for the very first property added to a root shape, parent is root shape, slot is 0.
    // If root shape had no property, next_slot for first prop is 0.
    if (parent == nullptr && property_name == nullptr) {
        // Root shape: first property slot_index is 0.
        // Subsequent properties on child shapes will increment.
        next_slot = (transitions.empty() && slot_index == 0 && property_name == nullptr) ? 0 : slot_index;
    }

    // Actually, if parent is non-null, this shape represents a property slot at slot_index.
    // The next property added to `this` will have slot_index = this->slot_index + 1 (if this shape has a property)
    // or slot_index = 0 if `this` is root shape.
    if (parent == nullptr && property_name == nullptr) {
        next_slot = 0;
    } else {
        next_slot = slot_index + 1;
    }

    out_slot = next_slot;
    Shape* next_shape = arena.create<Shape>(this, prop_str, next_slot, prototype);
    transitions.push_back(ShapeTransition{prop_str, next_shape});
    return next_shape;
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
