#pragma once

#include <cstdint>
#include <vector>

#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// Where a property lives and what KIND it is. A slot index alone was the
// whole answer while every property was a value in a shape-indexed slot;
// an accessor is a pair of functions to CALL, and both facts have to reach
// the inline caches, which must refuse to cache the second (docs/0019
// decision 5).
struct PropertyInfo {
    uint32_t slot{0};
    bool enumerable{true};
    // `slot` holds the getter and `slot + 1` the setter; either may be
    // `undefined` for a half-written accessor.
    bool accessor{false};
};

struct ShapeTransition {
    StringHeader* property_name{nullptr};
    class Shape* next_shape{nullptr};
    // Part of the KEY, not payload: `enumerable` and `accessor` are
    // attributes of the property, so two objects that added the same name
    // with different attributes have different layouts and must not share a
    // node. Every plain `{}` and every class prototype start from the one
    // root shape (docs/0008 decision 1), so a transition matched on name
    // alone would hand `o.m = 1` the non-enumerable node a class method left
    // behind — or, worse, the two-slot accessor node a getter left there,
    // after which a plain data read would return the getter function.
    bool enumerable{true};
    bool accessor{false};
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
    // decision 2), and for a class accessor, which 15.7.14 defines the same
    // way. An accessor in an OBJECT LITERAL is enumerable, like any other
    // property definition there.
    bool enumerable{true};
    // This node's property is an accessor pair occupying `slot_index` and
    // `slot_index + 1`, so the node is two slots wide and the next property
    // starts past both.
    bool accessor{false};
    Shape* root{nullptr};  // self, for a root shape
    Value prototype;       // meaningful on a root shape only
    std::vector<ShapeTransition> transitions;
    // Non-null on a DICTIONARY shape: a private, unshared shape belonging to
    // exactly one object, whose own properties live in the table rather than
    // in this chain (docs/0019 decision 1). `parent` is null and
    // `transitions` stays empty on one — a dictionary is a leaf that never
    // becomes anyone's parent.
    Dictionary* dict{nullptr};

    Shape() : root(this), prototype(Value::fromUndefined()) {}
    Shape(Shape* parent_shape, StringHeader* prop_name, uint32_t slot, Shape* root_shape,
          bool is_enumerable, bool is_accessor)
        : parent(parent_shape),
          property_name(prop_name),
          slot_index(slot),
          enumerable(is_enumerable),
          accessor(is_accessor),
          root(root_shape),
          prototype(Value::fromUndefined()) {}

    // Creates a root shape whose instances have `proto` as their
    // prototype. A root carrying a non-undefined prototype also has to be
    // handed to the collector, and only the owner of the arena knows
    // whether its lifetime matches a heap's — so registration is the
    // caller's job (see newRootShape in rt_helpers), not this one's.
    static Shape* createRoot(NonMovingArena& arena, Value proto = Value::fromUndefined());

    bool isDictionary() const noexcept { return dict != nullptr; }

    // How many slots this node's own property occupies.
    uint32_t slotWidth() const noexcept { return accessor ? 2u : 1u; }
    // The first slot a property added after this one may use. A root shape
    // owns no property, so the first property added to it takes slot 0.
    uint32_t nextSlotIndex() const noexcept {
        return property_name == nullptr ? 0u : slot_index + slotWidth();
    }

    Shape* addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name, uint32_t& out_slot,
                       bool is_enumerable = true, bool is_accessor = false);

    // Existence and location of an own property. The `out_slot` overload is
    // for callers that only ask "is it there, and where" — `in`, and the
    // unit tests that pin the transition tree.
    bool lookupProperty(StringHeader* name, PropertyInfo& out) const noexcept;
    bool lookupProperty(StringHeader* name, uint32_t& out_slot) const noexcept;

    // Own property names in INSERTION order. For a transition-tree shape the
    // chain already IS that order, newest first, so this walks to the root
    // and reverses — no per-object key list to allocate or keep in sync, and
    // objects sharing a shape share the answer (docs/0009 decision 1). For a
    // dictionary shape it is the entry vector, which is the same order kept
    // explicitly because a delete made the chain unable to keep it.
    //
    // `enumerableOnly` is what every ENUMERATION caller passes: `Object.keys`,
    // object spread and `for-in` all speak of own *enumerable* keys, and a
    // class method is not one.
    std::vector<StringHeader*> ownKeysInInsertionOrder(bool enumerableOnly = false) const;

    // Past this many own properties an object is a map, not a record, and
    // the transition tree is the wrong structure for it. Reaching it is a
    // named hard error: dictionary mode as built here is a linear entry
    // vector, which is not an answer for an object with a thousand
    // properties either (docs/0019, "not here").
    static constexpr uint32_t kDictionaryThreshold = 1024;

    Value prototypeValue() const noexcept { return root ? root->prototype : Value::fromUndefined(); }
};

}  // namespace bronze
