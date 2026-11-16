#pragma once

// THE SHAPE CELLS (il.h Module::shapeCells, abi bronze_register_shape_cells):
// a module's words that each name one shape by the harvest's name for it
// (type_feedback.h typeFeedbackShapeName: `<prototype>[#N]{k1,...,kn}`), which
// its speculated code compares receivers' shape words with (`shape.cell`).
//
// The module's entry registers them, zero, on each thread that runs it. A cell
// whose shape exists then is filled at once; the rest wait, and the first
// shape made later under that name fills its cell. A cell is written once and
// never again: shapes are immortal and never move, so the word stays the
// shape's for the life of the thread. A cell no shape fills stays zero, which
// no object's shape word is, so the code comparing with it always leaves.
//
// Only a shape whose every property a slot load may touch directly fills a
// cell: shared (no dictionary, nobody's prototype, no sentinel), every node a
// data property boxed in its slot, and no symbol key. A data property that is
// not writable is spelled `!k` in the name, so the speculated code knows not
// to store into it. A name that only such a shape could carry is what the
// speculated code assumed; an ineligible shape under the name leaves the cell
// to a later one.

#include <cstddef>
#include <cstdint>

namespace bronze {
class Shape;
}

namespace bronze::runtime {

// A shape node just made (a transition's child, or a root): fills the pending
// cells its name matches. Cheap when no cell is pending.
void rtShapeCellsNoteShape(const Shape* shape);

// A module going away: its pending cells are forgotten (their memory may be
// freed next). By the data range it occupied, or by its module epoch.
void rtShapeCellsDropWithin(const void* begin, size_t size);
void rtShapeCellsDropEpoch(uint64_t epoch);

}  // namespace bronze::runtime
