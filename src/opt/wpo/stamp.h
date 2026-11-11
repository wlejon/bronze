#pragma once

// The first consumer of the predictions (Solver in `predict` mode): a
// static-slot claim (il.h `staticSlot`) on every property site lowering left
// without one whose predicted receivers all hold the key at one slot.
//
// Every claim is a GUESS (`staticSlotGuessed`) and is checked at run time
// before it is used, by the guard il_static_slot.cpp puts in front of the
// site's inline cache:
//
//   identity  one predicted runtime shape: the site gets a cell of its own,
//             which the runtime fills once, and only with a shape that really
//             has the key as an own data property at the slot
//             (runtime/static_shape.cpp). A wrong guess leaves the refusal
//             marker in the cell after one call and the site never calls
//             again.
//   family    several shapes: a range of the module's class families
//             (il.h `classFamilies`) every member of which declares the key
//             at the slot. The runtime stamps a shape with a family only
//             after checking that family's fields are a prefix of it
//             (runtime/class_family.cpp), once per shape.
//
// Families are the ones lowering proved and registered; a site whose
// receivers no registered family describes is counted and left alone.
//
// A read whose few predicted receivers hold the key as an own data property
// at slots no one claim covers is marked polymorphic instead (il.h
// `icPolymorphic`): the backend scans the site's cache ways inline, which the
// runtime fills with the shapes it really meets.

#include <cstdint>
#include <string>
#include <vector>

#include "il/il.h"
#include "opt/wpo/census.h"
#include "opt/wpo/shape.h"
#include "opt/wpo/solver.h"

namespace bronze::wpo {

enum class StampForm : uint8_t { None, Identity, Family };

struct StampEntry {
    uint32_t fn = 0;
    uint32_t block = 0;
    uint32_t inst = 0;
    uint32_t slot = 0;
    StampForm form = StampForm::None;
    uint32_t familyLo = 0;
    uint32_t familySpan = 0;
    // Identity: the receiver's shape is not certain (il.h staticRegrow).
    bool regrow = false;
};

// A method call's predicted single callee (il.h `directTarget`): the
// backend's direct edge compares the code pointer the site's method cache
// found against this function's, so a wrong prediction takes the cache's
// ordinary dispatch.
struct DirectEntry {
    uint32_t fn = 0;
    uint32_t block = 0;
    uint32_t inst = 0;
    uint32_t target = 0;
};

// A read marked polymorphic.
struct PolyEntry {
    uint32_t fn = 0;
    uint32_t block = 0;
    uint32_t inst = 0;
};

struct StampPlan {
    std::vector<StampEntry> entries;
    std::vector<DirectEntry> directs;
    std::vector<PolyEntry> polys;
    // Reads some predicted receiver answers with `undefined` (il.h `icAbsent`).
    std::vector<PolyEntry> absents;
    // Method calls whose one predicted callee a receiver holds in an own slot.
    std::vector<DirectEntry> ownCalls;
    // Property sites whose predicted receivers resolve to one prototype getter or setter.
    std::vector<DirectEntry> accessorCalls;
    // Method calls whose direct target lowering guessed (by the method's name
    // on the receiver's class) where the prediction gives the site one other
    // callee: the guess is withdrawn, since every receiver the prediction saw
    // misses its guard, and the site is planned as one without a target.
    std::vector<PolyEntry> withdrawn;
    // Reads left without a claim whose predicted receivers all hold the key
    // as an own data property, and how many of those have more receivers than
    // the cache has ways.
    uint32_t polyCandidates = 0;
    uint32_t polyTooMany = 0;
    // Method calls with one predicted IL callee and no direct target, and how
    // many of those the callee's shape refuses (an `arguments` object, a rest
    // array, an environment, a typed parameter a short call would pad).
    uint32_t callCandidates = 0;
    uint32_t callRefused = 0;
    // ... and how many a receiver may hold the callee in an own slot.
    uint32_t callOwnHeld = 0;
    // ... and how many of the candidates had a guess of lowering's withdrawn.
    uint32_t callWithdrawn = 0;
    // Sites whose predicted receivers agree on a slot, and why the ones
    // without a stamp have none.
    uint32_t candidates = 0;
    // Identity claims on one allocation site whose shape the prediction does
    // not know exactly (it is extended after it is exposed): one cell, the
    // shape its objects most likely share.
    uint32_t identityInexact = 0;
    uint32_t noFamilies = 0;      // the module registers no class family
    uint32_t noStamp = 0;         // a receiver no registered family describes
    uint32_t rangeMismatch = 0;   // the families between them disagree on the slot
};

// Decides the stamps from the predicted facts (classifySites over the
// predicting solver). Reads the module; changes nothing.
StampPlan planStamps(const Solver& predicted, const ShapeAnalysis& shapes, const std::vector<SiteFact>& facts);

// Writes the plan's claims into `mod`, handing each identity site a new
// static cell, and the direct targets and polymorphic marks.
void applyStamps(il::Module& mod, const StampPlan& plan);

// Writes only the plan's direct targets (and withdraws the guesses it
// contradicts).
void applyDirects(il::Module& mod, const StampPlan& plan);

// The family range a receiver whose own keys are exactly `slots` (key indices,
// in slot order) may be stamped with, when every family in the range declares
// `keyIndex` at `slot` (writable for a write): what a site reading or writing
// that key of such a receiver can claim (StampForm::Family). False when no
// registered family begins the keys, or the range disagrees on the slot.
bool familyClaimFor(const il::Module& mod, const std::vector<uint32_t>& slots, uint32_t slot, uint32_t keyIndex,
                    bool write, uint32_t* lo, uint32_t* span);

}  // namespace bronze::wpo
