#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/property_key.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// Where a property lives and what KIND it is. A slot index alone was the whole
// answer while every property was a value in a shape-indexed slot; an accessor
// is a pair of functions to CALL, and both facts have to reach the inline
// caches, which must refuse to cache the second.
struct PropertyInfo {
    uint32_t slot{0};
    bool enumerable{true};
    // `slot` holds the getter and `slot + 1` the setter; either may be
    // `undefined` for a half-written accessor.
    bool accessor{false};
    // True for every property in a shape chain: the two attributes a transition
    // key does not carry are the DEFAULTS there, and an object that wants
    // either of them false is in dictionary mode.
    bool writable{true};
    bool configurable{true};
};

struct ShapeTransition {
    PropertyKey key;
    class Shape* next_shape{nullptr};
    // Part of the KEY, not payload: `enumerable` and `accessor` are attributes
    // of the property, so two objects that added the same name with different
    // attributes have different layouts and must not share a node. Every plain
    // `{}` and every class prototype start from the one root shape, so a
    // transition matched on name alone would hand `o.m = 1` the non-enumerable
    // node a class method left behind — or, worse, the two-slot accessor node a
    // getter left there, after which a plain data read would return the getter
    // function.
    bool enumerable{true};
    bool accessor{false};
    bool writable{true};
    bool configurable{true};
};

// A hidden class: one node of a transition tree, immortal and non-moving, so
// shape identity is a raw pointer compare for the life of the program and IC
// words never need GC fixup.
//
// The prototype is recorded on the SHAPE rather than on the object, which is
// what makes a proto-hit IC sound: matching the receiver's shape then implies
// its prototype. Only the ROOT of a transition tree stores it — children reach
// it through `root` — so the collector forwards one slot per distinct prototype
// rather than one per shape.
class Shape {
public:
    Shape* parent{nullptr};
    // The own property this node adds: a string key or a symbol key, matched
    // by PropertyKey's one rule. Invalid on a root shape, which owns none.
    PropertyKey key;
    uint32_t slot_index{0};
    // Whether the property this node owns is visible to enumeration —
    // `Object.keys`, object spread, and `for-in`. False for a class method,
    // which ECMA-262 15.7.14 defines with `enumerable: false`, and for a class
    // accessor, which 15.7.14 defines the same way. An accessor in an OBJECT
    // LITERAL is enumerable, like any other property definition there.
    bool enumerable{true};
    // This node's property is an accessor pair occupying `slot_index` and
    // `slot_index + 1`, so the node is two slots wide and the next property
    // starts past both.
    bool accessor{false};
    bool writable{true};
    bool configurable{true};
    Shape* root{nullptr};  // self, for a root shape
    Value prototype;       // meaningful on a root shape only
    // Non-null on a DICTIONARY shape: a private, unshared shape belonging to
    // exactly one object, whose own properties live in the table rather than in
    // this chain. `parent` is null and `transitions` stays empty on one — a
    // dictionary is a leaf that never becomes anyone's parent.
    Dictionary* dict{nullptr};
    // Some object AT this shape is the prototype recorded on some root shape,
    // so adding a property to such an object shadows what inline-cache entries
    // BELOW it point at. Only adds from a marked shape bump the
    // prototype-mutation epoch, which is what keeps ordinary object
    // construction from invalidating every proto cache in the program.
    //
    // It is a property of the SHAPE and shapes are shared, so an object that
    // merely has the same layout as a prototype is marked too. That direction
    // is safe — it can only cost an extra miss — and the direction that would
    // not be, a prototype whose shape is unmarked, is what `addProperty`'s
    // propagation and `createRoot`'s marking exist to rule out.
    bool used_as_prototype{false};
    // Which proven class layout this shape's own properties BEGIN with, as the
    // process-wide id `bronze_family_stamp` assigns — or `UNSTAMPED` (never
    // asked) or `NONE` (asked, and no registered class's field list is a prefix
    // of this shape's properties).
    //
    // A fact about the shape, verified against the shape: the stamper walks
    // this chain and compares every name, slot and attribute against the
    // class's declared layout before it writes anything here. That is what lets
    // a site in `Object3D.updateMatrixWorld` load `this.matrixWorld` at a
    // constant offset from a Group, a Mesh and a Scene alike — the three have
    // three shapes, and all three were checked to start with Object3D's fields.
    //
    // Written once and never invalidated, which is sound because every way a
    // property's name, slot or attributes can change under an object either
    // transitions it to a different shape or drops it into dictionary mode
    // (`delete`, `Object.freeze`, a writable:true->false redefinition,
    // `setPrototypeOf`). A dictionary shape is never stamped.
    uint64_t family_stamp{BRONZE_ABI_FAMILY_UNSTAMPED};
    // LAST, and that position is load-bearing: generated code reads every
    // field above this one (the ABI offsets in bronze_abi.h, pinned below),
    // and a standard-library type's size may differ between build
    // configurations — so nothing the backend reads may sit after one.
    std::vector<ShapeTransition> transitions;

    Shape() : root(this), prototype(Value::fromUndefined()) {}
    Shape(Shape* parent_shape, PropertyKey prop_key, uint32_t slot, Shape* root_shape,
          bool is_enumerable, bool is_accessor, bool is_writable = true, bool is_configurable = true)
        : parent(parent_shape),
          key(prop_key),
          slot_index(slot),
          enumerable(is_enumerable),
          accessor(is_accessor),
          writable(is_writable),
          configurable(is_configurable),
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
        return !key.valid() ? 0u : slot_index + slotWidth();
    }

    Shape* addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name, uint32_t& out_slot,
                       bool is_enumerable = true, bool is_accessor = false,
                       bool is_writable = true, bool is_configurable = true);

    // Existence and location of an own property. The `out_slot` overload is
    // for callers that only ask "is it there, and where" — `in`, and the
    // unit tests that pin the transition tree.
    bool lookupProperty(PropertyKey name, PropertyInfo& out) const noexcept;
    bool lookupProperty(PropertyKey name, uint32_t& out_slot) const noexcept;

    // Own property names in INSERTION order. For a transition-tree shape the
    // chain already IS that order, newest first, so this walks to the root and
    // reverses — no per-object key list to allocate or keep in sync, and
    // objects sharing a shape share the answer. For a dictionary shape it is
    // the entry vector, which is the same order kept explicitly because a
    // delete made the chain unable to keep it.
    //
    // `enumerableOnly` is what every ENUMERATION caller passes: `Object.keys`,
    // object spread and `for-in` all speak of own *enumerable* keys, and a
    // class method is not one.
    //
    // BOTH kinds of key come back, in one list, because insertion order is one
    // order — 6.1.7.1's "symbols after strings" is a rule about the ANSWER a
    // caller builds, and `rtOwnKeysOrdered` is where it is applied. A caller
    // that only wants string keys says so there rather than trusting this to
    // have filtered for it.
    std::vector<PropertyKey> ownKeysInInsertionOrder(bool enumerableOnly = false) const;

    // Past this many own properties an object is a map, not a record, and
    // the transition tree is the wrong structure for it. Reaching it is a
    // named hard error: dictionary mode as built here is a linear entry
    // vector, which is not an answer for an object with a thousand
    // properties either ("not here").
    static constexpr uint32_t kDictionaryThreshold = 65536;

    Value prototypeValue() const noexcept { return root ? root->prototype : Value::fromUndefined(); }
};

// Part of the generated-code ABI: the depth > 0 proto-hit read walks
// shape -> root -> prototype and refuses a dictionary inline, and the
// shape-transition write hit reads parent, slot_index, the attribute bytes
// and the prototype mark. Pinned HERE, beside the type that owns the layout,
// so a reorder breaks the build instead of miscompiling every cached access.
static_assert(offsetof(Shape, parent) == BRONZE_ABI_SHAPE_PARENT_OFFSET);
static_assert(offsetof(Shape, slot_index) == BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET);
static_assert(offsetof(Shape, enumerable) == BRONZE_ABI_SHAPE_ATTRS_OFFSET);
static_assert(offsetof(Shape, accessor) == BRONZE_ABI_SHAPE_ATTRS_OFFSET + 1 &&
              offsetof(Shape, writable) == BRONZE_ABI_SHAPE_ATTRS_OFFSET + 2 &&
              offsetof(Shape, configurable) == BRONZE_ABI_SHAPE_ATTRS_OFFSET + 3,
              "the write fast path reads the four attribute bools as one little-endian u32");
static_assert(sizeof(Shape::enumerable) == 1 && sizeof(Shape::accessor) == 1 &&
              sizeof(Shape::writable) == 1 && sizeof(Shape::configurable) == 1);
static_assert(offsetof(Shape, root) == BRONZE_ABI_SHAPE_ROOT_OFFSET);
static_assert(offsetof(Shape, prototype) == BRONZE_ABI_SHAPE_PROTO_OFFSET);
static_assert(offsetof(Shape, dict) == BRONZE_ABI_SHAPE_DICT_OFFSET);
static_assert(offsetof(Shape, used_as_prototype) == BRONZE_ABI_SHAPE_USEDPROTO_OFFSET);
static_assert(offsetof(Shape, family_stamp) == BRONZE_ABI_SHAPE_FAMILY_OFFSET);

}  // namespace bronze
