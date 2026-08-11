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
    // Part of the KEY, not payload: `enumerable` is an attribute of the
    // property, so two objects that added the same name with different
    // attributes have different layouts and must not share a node. Every
    // plain `{}` and every class prototype start from the one root shape
    // (docs/0008 decision 1), so a transition matched on name alone would
    // hand `o.m = 1` the non-enumerable node a class method left behind.
    bool enumerable{true};
};

// A hidden class: one node of a transition tree, immortal and non-moving
// (docs/0004 decision 2), so shape identity is a raw pointer compare for
// the life of the program and IC words never need GC fixup.
//
// The prototype is recorded on the SHAPE rather than on the object, which
// is what makes a proto-hit IC sound: matching the receiver's shape then
// implies its prototype (docs/0008 decision 1). Only the ROOT of a
// transition tree stores it — children reach it through `root` — so the
// collector forwards one slot per distinct prototype rather than one per
// shape.
class Shape {
public:
    Shape* parent{nullptr};
    StringHeader* property_name{nullptr};
    uint32_t slot_index{0};
    // Whether the property this node owns is visible to enumeration —
    // `Object.keys`, object spread, and `for-in`. False for a class method,
    // which ECMA-262 15.7.14 defines with `enumerable: false` (docs/0018
    // decision 2). Ordinary assignment always creates an enumerable property.
    bool enumerable{true};
    Shape* root{nullptr};  // self, for a root shape
    Value prototype;       // meaningful on a root shape only
    std::vector<ShapeTransition> transitions;

    Shape() : root(this), prototype(Value::fromUndefined()) {}
    Shape(Shape* parent_shape, StringHeader* prop_name, uint32_t slot, Shape* root_shape,
          bool is_enumerable)
        : parent(parent_shape),
          property_name(prop_name),
          slot_index(slot),
          enumerable(is_enumerable),
          root(root_shape),
          prototype(Value::fromUndefined()) {}

    // Creates a root shape whose instances have `proto` as their
    // prototype. A root carrying a non-undefined prototype also has to be
    // handed to the collector, and only the owner of the arena knows
    // whether its lifetime matches a heap's — so registration is the
    // caller's job (see newRootShape in rt_helpers), not this one's.
    static Shape* createRoot(NonMovingArena& arena, Value proto = Value::fromUndefined());

    Shape* addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name, uint32_t& out_slot,
                       bool is_enumerable = true);

    bool lookupProperty(StringHeader* name, uint32_t& out_slot) const noexcept;

    // Own property names in INSERTION order. The transition chain already
    // is that order, newest first, so this walks to the root and reverses
    // — no per-object key list to allocate or keep in sync, and objects
    // sharing a shape share the answer (docs/0009 decision 1). The
    // StringHeaders are arena-interned and immortal.
    //
    // `enumerableOnly` is what every ENUMERATION caller passes: `Object.keys`,
    // object spread and `for-in` all speak of own *enumerable* keys, and a
    // class method is not one.
    std::vector<StringHeader*> ownKeysInInsertionOrder(bool enumerableOnly = false) const;

    // Past this many own properties an object is a map, not a record, and
    // the transition tree is the wrong structure for it. Reaching it is a
    // named hard error until dictionary mode lands (docs/0009 decision 3).
    static constexpr uint32_t kDictionaryThreshold = 1024;

    Value prototypeValue() const noexcept { return root ? root->prototype : Value::fromUndefined(); }
};

}  // namespace bronze
