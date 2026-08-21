#pragma once

#include <cstdint>

#include "runtime/shape.h"

// The layout-family registry: what a compiled module tells the runtime about
// the classes whose construction sequence it modelled, and what the runtime
// does with that when it meets a shape.
//
// Everything a consumer needs is `bronze_register_class_family` and
// `bronze_family_stamp` (declared in the ABI registry). This header exists for
// the doctests, which have to ask questions no JS program can see — did this
// shape get stamped, and with which class — and for the seam that clears the
// registry between them.

namespace bronze::runtime {

// The id `bronze_family_stamp` would write for `shape`, computed without
// touching it. Tests only; the helper itself does not go through here.
uint64_t classFamilyIdFor(Shape* shape);

// How many classes are registered, across every module. Tests and the IC log.
uint32_t classFamilyCount();

// Forgets every registration and resets the id counter. Tests only: a process
// that has run a module cannot un-run it, and generated code never calls this.
void classFamilyResetForTesting();

}  // namespace bronze::runtime
