#pragma once

#include <cstdint>
#include <vector>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

struct ShapeTransition {
    StringHeader* property_name{nullptr};
    class Shape* next_shape{nullptr};
};

class Shape {
public:
    Shape* parent{nullptr};
    StringHeader* property_name{nullptr};
    uint32_t slot_index{0};
    Shape* prototype{nullptr};
    std::vector<ShapeTransition> transitions;

    Shape() = default;
    Shape(Shape* parent_shape, StringHeader* prop_name, uint32_t slot, Shape* proto)
        : parent(parent_shape), property_name(prop_name), slot_index(slot), prototype(proto) {}

    static Shape* createRoot(NonMovingArena& arena, Shape* prototype = nullptr);

    Shape* addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name, uint32_t& out_slot);

    bool lookupProperty(StringHeader* name, uint32_t& out_slot) const noexcept;
};

}  // namespace bronze
