// A computed read's SITE CELL (codegen-brass/elem_cells.h; the words are
// bronze_abi_layout.h's BRONZE_ABI_ELEM_CELL_WORDS): shape, key bits, slot
// word, fills, validity. Generated code answers `o[k]` from it with two
// compares where the computed-read table (elem_ic.h) costs a hash and a probe
// and a dictionary a hash of the key's string: pixi reads
// `renderPipes[renderable.renderPipeId]` once a sprite a frame, `_gpuData[uid]`
// of an id-keyed map, and `gl[name]` of a method on the prototype, and every
// one of those reads names one pair.
//
// bronze_elem_get_cell is the read the site makes when the cell does not
// answer it: it answers the read and, while the site still takes fills,
// fills the cell from the same lookup. It runs no code of the program's and
// allocates nothing a collection would see before it hands the read it does
// not answer to bronze_elem_get.
//
// What each form stands for, each a thing checked here:
//   - the receiver: a plain object without internal slots, at the cell's
//     shape; a shape names its own properties for good, and a dictionary's
//     shape belongs to its one object;
//   - the key, by its bits: a Number's are the Number (`sprite._gpuData[uid]`
//     names a string property by one, and the cell answers it without the
//     string), and of strings an ARENA string only, which is immortal, so its
//     bits name the same characters for as long as the cell holds them. A
//     heap string's bits do not survive a collection, and the collector does
//     not sweep cells;
//   - an own slot of a shape: nothing more;
//   - a slot 1..BRONZE_ABI_ELEM_CELL_MAX_DEPTH links up: the computed-read
//     table's entry (the property path's own rules), valid while the
//     prototype epoch is the one it was filled at;
//   - an own slot of a dictionary: the table's entry, a data property, valid
//     while the table's generation is the one it was found at (a removal or a
//     redefinition to other storage moves it; dictionary.h). Generations are
//     unique on the thread, so a table that took a dead object's shape
//     address is never mistaken for the one the cell saw.
//
// A site stops asking at BRONZE_ABI_ELEM_CELL_LIMIT and reads through the
// table from then on: after eight fills (it names many pairs), after a
// thousand reads of a kind the cell never stands for (a heap string's key, a
// receiver that is no plain object), or after a million calls that filled
// nothing for another reason (a pair the table does not hold yet), which
// would otherwise call here at every read for good. A thousand of those was
// too few: pixi_sprites' ten thousand sprites pass a site several times
// before the pair it will read from then on can be filled.

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/dictionary.h"
#include "runtime/elem_ic.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

constexpr uint64_t kDictSlotWord = uint64_t{BRONZE_ABI_ELEM_CELL_DICT} << 32;
static_assert(BRONZE_ABI_ELEM_CELL_DICT > BRONZE_ABI_ELEM_CELL_MAX_DEPTH, "the dictionary form is no depth");

void fill(uint64_t* cell, const Shape* shape, uint64_t keyBits, uint64_t slotWord, uint64_t validity) {
    // The shape word last after clearing it first: a cell reads as a pair
    // only once every word of the pair is written.
    cell[0] = 0;
    cell[1] = keyBits;
    cell[2] = slotWord;
    cell[4] = validity;
    cell[0] = reinterpret_cast<uint64_t>(shape);
    cell[3] += BRONZE_ABI_ELEM_CELL_FILL - 1;
}

// The read, answered and the cell filled, or false (and the read is the
// generic one's).
bool readFilling(uint64_t objBits, uint64_t keyBits, uint64_t* cell, uint64_t& out) {
    ++cell[3];
    const auto never = [&] {
        cell[3] += (BRONZE_ABI_ELEM_CELL_LIMIT >> 10) - 1;
        return false;
    };
    const Value key(keyBits);
    const bool arena = key.isString() && key.asString<StringHeader>()->isArena();
    if (!arena && !key.isNumber()) return never();
    const Value obj(objBits);
    if (!obj.isObject() || obj.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return never();
    auto* plain = obj.asObject<ObjectHeader>();
    Shape* shape = plain->shape;
    if (shape == nullptr || plain->internalSlotCount() != 0 || rtMaybeGlobalObjectShape(shape)) return never();

    if (shape->isDictionary()) {
        PropertyKey name;
        if (arena) {
            name = PropertyKey::forString(key.asString<StringHeader>());
        } else {
            // A Number names its ToString; the small ones are immortal
            // strings of their own (rtSmallIntString), anything else is the
            // generic read's.
            const double d = key.asNumber();
            if (!(d >= 0.0 && d < 4294967296.0)) return false;
            const auto n = static_cast<uint32_t>(d);
            if (static_cast<double>(n) != d) return false;
            const Value text = rtSmallIntString(n);
            if (!text.isString()) return false;
            name = PropertyKey::forString(text.asString<StringHeader>());
        }
        const DictEntry* e = shape->dict->find(name);
        if (e == nullptr || e->accessor) return false;
        out = plain->getSlot(e->slot).rawBits();
        fill(cell, shape, keyBits, e->slot | kDictSlotWord, shape->dict->generation);
        return true;
    }

    PropertyInfo info;
    if (arena) {
        // The shape answers for a name it holds: no read need have come before.
        if (shape->lookupProperty(PropertyKey::forString(key.asString<StringHeader>()), info)) {
            if (info.accessor) return never();
            out = plain->getSlot(info.slot).rawBits();
            fill(cell, shape, keyBits, info.slot, 0);
            return true;
        }
    }
    // What the table holds for the pair, from the read before this one: an
    // own slot (a Number key), or one up the chain.
    const ElemProbe probe = elemCacheProbe(obj, key);
    if (!probe.hit || probe.entry == nullptr || probe.entry->ic.cached_shape != shape) return false;
    if (probe.kind != (arena ? ElemKeyKind::String : ElemKeyKind::Number)) return false;
    const InlineCache& ic = probe.entry->ic;
    if (ic.isAbsent() || ic.isAccessor() || ic.isStatics()) return false;
    const uint32_t depth = ic.realDepth();
    if (depth > BRONZE_ABI_ELEM_CELL_MAX_DEPTH) return false;
    out = probe.value.rawBits();
    fill(cell, shape, keyBits, ic.cached_slot | (uint64_t{depth} << 32), depth == 0 ? 0 : ic.cached_epoch);
    return true;
}

}  // namespace

}  // namespace bronze::runtime

extern "C" uint64_t bronze_elem_get_cell(uint64_t objBits, uint64_t keyBits, uint64_t* cell) {
    using namespace bronze::runtime;
    if (cell != nullptr && cell[3] < BRONZE_ABI_ELEM_CELL_LIMIT) {
        if (uint64_t out = 0; readFilling(objBits, keyBits, cell, out)) return out;
    }
    return bronze_elem_get(objBits, keyBits);
}
