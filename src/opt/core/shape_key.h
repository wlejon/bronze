#pragma once

// SHAPE NAMES, as a profile spells them, and the slot a key has in the shape
// a name stands for.
//
// The recorder and the harvest name a plain object's shape
// (runtime/type_feedback.cpp `shapeText`)
//
//     <prototype>[#<inline capacity>]{k1,k2,...}
//
// with the own keys in the order they were added, every one of them, and the
// `#N` part only for an inline capacity other than the default
// (BRONZE_ABI_OBJ_INLINE_SLOTS). A key is spelled `!k` when it is a data
// property that is not writable, `&k` when it is an accessor pair and `%k`
// when its slot is a DOUBLE one (runtime/slot_repr.h: the slot holds a Number,
// and every store keeps it so or moves the object to another shape). A
// dictionary is `<prototype>{dict}`; a receiver that is no plain object is
// named by its kind (`array`, `function`, `number`, ...) and has no braces at
// all.
//
// THE SLOT RULE. Shape::placeSlot (runtime/shape.h) gives the K-th property of
// an object with N inline Values slot K when K < N and
// BRONZE_ABI_OBJ_OVERFLOW_SLOT + (K - N) otherwise, PROVIDED every property
// before it is one slot wide: an accessor pair takes two. That is the slot
// `slot.load` / `slot.store` are given (il_op.h numbers slots the same way).
//
// THE LOADER CONTRACT this rests on (Module::shapeCells, il.h). A cell's key is
// the name verbatim, and the module loader must resolve a key only to a shape
// that
//   - has exactly that name, and no other shape it could pick with that name
//     has another key sequence (a key holding `,` `{` or `}` can make two key
//     lists print alike: an ambiguous name resolves to nothing);
//   - is not a dictionary;
//   - has every own property a DATA property with the attributes an
//     assignment creates (writable, enumerable, configurable), so each is one
//     slot wide, a read is the slot and a write to it is permitted;
//   - keeps every slot in the representation its key's spelling says: a
//     double slot for `%k` and a boxed one otherwise (never an object slot,
//     docs/slot-representation.md). A boxed slot holds a Value and any Value
//     may be stored there; a double slot holds a Number, so a read of it is
//     a Number and only a Number may be stored there.
// A key it cannot resolve leaves the cell holding a word no object has, and
// every guard against it fails. So a profile that is wrong costs exits, never
// a wrong value.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::opt {

struct ShapeName {
    std::string prototype;     // the part before `#` / `{`
    uint32_t inlineSlots = 0;  // N: the `#N` capacity, else the default
    std::vector<std::string> keys;  // without their `!`
    std::vector<bool> readOnly;     // per key: spelled `!k`
    std::vector<bool> number;       // per key: spelled `%k`, a double slot
};

// Parses a plain-object shape name. Nullopt for anything this step will not
// speculate on: a kind word, `*other*`, a dictionary, a symbol key
// (`@@symbol`, which names no particular symbol), an accessor key (`&k`,
// two slots wide), an empty key, more than one `{`, a missing closing `}`,
// or a capacity past BRONZE_ABI_OBJ_OVERFLOW_SLOT.
std::optional<ShapeName> parseShapeName(std::string_view name);

// The name of the shape with the first `keyCount` keys of `shape`, spelled as
// the runtime spells it.
std::string shapeNameWithKeys(const ShapeName& shape, size_t keyCount);

// The slot of `key` in a shape so named, by the slot rule; nullopt when the
// key is not one of its own keys or appears more than once (which only an
// ambiguous name can show). A read may load it.
std::optional<int32_t> ownSlot(const ShapeName& shape, std::string_view key);
// ownSlot for a store: nullopt as well when the key is not writable.
std::optional<int32_t> writableSlot(const ShapeName& shape, std::string_view key);
// Whether the key's slot is a double one (spelled `%k`): a read of it is a
// Number, and a store must bring one.
bool numberSlot(const ShapeName& shape, std::string_view key);

// Whether a property key is one a shape can hold at all: not empty and not a
// canonical array index (`"0"`, `"17"`), which an object keeps among its
// elements on the paths that use one.
bool isNamedKey(std::string_view key);

}  // namespace bronze::opt
