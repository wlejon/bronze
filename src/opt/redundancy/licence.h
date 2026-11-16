#pragma once

// LICENCES: what a GUARDED instruction (il/effects.h `guarded`) needs to be
// legal, expressed as a fact the FactEnv can be asked about.
//
// il_op.h names each unchecked form's precondition ("legal only on a
// `fn.code_is` true edge", ...). The effect table only says that there is
// one; this file says which, for the forms with a result that loop hoisting
// may move. It is the one place a pass reads them from. (A candidate for
// opt/core beside edge_facts.h, whose facts these are.)
//
// A licence is recorded at the instruction's ORIGINAL position, from the
// facts that hold there, and must hold where the instruction is moved to: the
// same callee, a shape among the same cells. Holding "some" shape is not
// enough — a slot number is only meaningful under the shapes that placed it,
// and reading slot 7 of an object whose real shape has five would read past
// its storage.
//
// Forms without a licence here (`elem.load`, whose bounds test is about an
// index that usually varies per iteration, and the stores) never move.

#include <cstdint>
#include <optional>
#include <vector>

#include "il/effects.h"
#include "il/il.h"
#include "opt/core/fact_env.h"

namespace bronze::opt::redundancy {

enum class LicenceKind : uint8_t {
    Object,  // the subject is a heap object (`load.shape`)
    Shape,   // the subject's shape is one of `cells` (`slot.load`)
    Callee,  // the subject is a closure of module function `imm` (`fn.env`)
    Array,   // the subject is an Array (`array.length`)
    TypedArray,  // the subject is a typed array (`typed.length`)
};

struct Licence {
    LicenceKind kind = LicenceKind::Object;
    ValueId subject = kNoValue;
    int64_t imm = 0;
    // The locations whose write could end the licence: a loop that writes
    // them may not have the instruction hoisted out of it, even when the
    // licence holds at the preheader.
    il::LocSet dependsOn;
    // Shape: the cells the subject may be at the original position. Each
    // places the slot as the read needs, so the read is legal wherever the
    // subject is known to be one of them.
    std::vector<uint32_t> cells;
};

// The licence `inst` holds under `facts` at its position, or nullopt when the
// facts do not show one (or `inst` is not a movable guarded form).
std::optional<Licence> licenceAt(const il::Instruction& inst, const FactEnv& facts);

// Whether `facts` grant `licence` exactly.
bool licenceHolds(const Licence& licence, const FactEnv& facts);

}  // namespace bronze::opt::redundancy
