#pragma once

// LEAVING A SHAPE: every way an object's shape pointer moves away from a shape
// it already had. Shapes are immutable and immortal (shape.h), so a fact about
// a receiver's layout can only stop being true through the receiver itself
// moving, and each runtime path that moves one calls `noteShapeLeave` at the
// move.
//
// Two kinds of move, and only one of them BREAKS a layout:
//
//   Add          an append on the transition tree. The child describes every
//                slot the parent did, at the same index with the same
//                attributes and representation, so nothing a compiled read or
//                write of an existing slot relied on has changed. Generated
//                code appends too (il2mir il_ic_lowering.cpp's transition
//                store, il_transition_run.cpp, il_define_add.cpp), always
//                along an existing edge, and needs no hook: the watched bit is
//                closed under descent (shape.h `watched`).
//   the rest     the object's slots change meaning: a dictionary renumbers or
//                re-attributes them, a generalization re-types one, a private
//                prototype chain routes its writes through the runtime. Only
//                the runtime does these, and each bumps the layout epoch when
//                the shape left is WATCHED (docs/aot-specialization.md, "S1").
//
// The layout epoch IS the prototype-mutation epoch (`bumpProtoMutationEpoch`,
// TLS `proto_epoch`): one word, already read inline, that compiled code
// compares to know that neither a prototype chain nor a watched layout moved.
//
// Not a leave: an object's first shape (allocation), a dictionary object's
// table edits (a dictionary shape is private and no compiled read names an
// unfrozen one, shape.h `readCacheable`), and an accessor pair rewritten in
// place.
//
// The shape census's leave log (docs/shape-census.md, "Leaves") sees every
// move; appends it records itself, at the write.

#include <cstdint>

#include "runtime/profile.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"

namespace bronze {

enum class ShapeLeave : uint8_t {
    Add = 0,           // a property added on the transition tree
    Generalize = 1,    // a double slot widened to boxed (slot_repr.h)
    Delete = 2,        // `delete` demoted the object to a dictionary
    Redefine = 3,      // an attribute change / redefinition the chain cannot express
    Integrity = 4,     // freeze / seal / preventExtensions
    SetPrototype = 5,  // [[SetPrototypeOf]]
    Threshold = 6,     // the add past Shape::kDictionaryThreshold
    BecomePrototype = 7,  // moved to a private marked chain (Shape::markObjectAsPrototype)
};
inline constexpr uint32_t kShapeLeaveKinds = 8;
const char* shapeLeaveName(ShapeLeave why);

namespace runtime {

// The out-of-line halves: census recording, and the bump for a broken
// watched layout.
void shapeLeaveSlow(const Shape* from, const Shape* to, ShapeLeave why);
void layoutBroken(const Shape* from, ShapeLeave why);

// `why` is a constant at every call site, so on an append both tests fold away
// and an add pays nothing: the census logs appends from its own record of the
// write that finds its key absent (shape_census.cpp), which costs nothing with
// the census off. Measured: a flag test here cost a runtime-add-heavy
// microbench 0.1% of its instructions.
inline void noteShapeLeave(const Shape* from, const Shape* to, ShapeLeave why) {
    if (why != ShapeLeave::Add && BRONZE_UNLIKELY(g_shapeCensusEnabled)) shapeLeaveSlow(from, to, why);
    if (why != ShapeLeave::Add && BRONZE_UNLIKELY(from->watched)) layoutBroken(from, why);
}

// Why the dictionary conversion in progress happens: `toDictionary` is
// reached from eight places and only its caller knows the reason, so the
// caller names it for the duration of the call. Unset, a conversion counts as
// a `Redefine`.
struct ShapeLeaveReason {
    explicit ShapeLeaveReason(ShapeLeave why);
    ~ShapeLeaveReason();
    ShapeLeaveReason(const ShapeLeaveReason&) = delete;
    ShapeLeaveReason& operator=(const ShapeLeaveReason&) = delete;
    uint8_t saved;
};
ShapeLeave currentDictionaryReason();

}  // namespace runtime
}  // namespace bronze
