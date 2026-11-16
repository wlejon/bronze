#pragma once

// NUMBER SLOTS FROM A HARVEST (docs/slot-representation.md, "From a harvest").
//
// A harvest (ic_harvest.h) is taken from a build that made no key eligible
// for a double slot, so its shapes are all boxed and its names spell no `%`.
// It says, per receiver name, the key positions born a Number and found
// holding one in every object of that shape (`doubles` rows), and every key
// anything else was found under (`mixed`). The build reads two things off it:
//
//   - the ELIGIBLE keys: a key at some `doubles` position that is not mixed
//     and that this module names (a key constant). The module registers them
//     at init (Module::numberSlotKeys, bronze_register_slot_repr), so the
//     runtime gives a slot added under one of them a double node when the
//     add stores a Number (runtime/object.cpp desiredSlotRepr);
//   - the names the run's shapes will have under that registration: the
//     harvest's receiver names respelled, each eligible key at one of its
//     shape's `doubles` positions as `%k`. Every cell, chain and set the
//     speculation builds from the harvest uses those names.
//
// A wrong guess costs time only: a name no shape of the run carries leaves
// its cell empty (its guard fails and S takes the generic arm or leaves), and
// a key that turns out mixed moves each object that meets a non-Number off
// its shape once (Shape::withSlotBoxed), after which the double edge is
// sticky-boxed and later objects are born boxed.

#include <set>
#include <string>

#include "il/il.h"
#include "opt/spec/profile.h"

namespace bronze::opt::speculate {

// The eligible keys: at a `doubles` position of some receiver, not mixed, and
// a key constant of `module`.
std::set<std::string> numberSlotKeys(const spec::Harvest& harvest, const il::Module& module);

// `harvest` with every receiver name (ways, `last` rows) spelled as the run
// with `keys` eligible names its shapes.
spec::Harvest spellNumberSlots(const spec::Harvest& harvest, const std::set<std::string>& keys);

}  // namespace bronze::opt::speculate
