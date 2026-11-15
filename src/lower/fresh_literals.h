#pragma once

#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::lower {

// LITERALS NOBODY HOLDS: an object or array literal whose every use is one of
// its own initializing stores, a read of a key one of those stores wrote, or
// its being stored as a field of another such literal, is never an object at
// run time. Each read is the value its store wrote, and the allocation and
// the stores go.
//
// `[a, b] = [b, a]`, `const { x, y } = { x: f(), y: g() }`, a literal built to
// name a few values and read back, and what is left of an options literal
// once the function taking it was inlined are all this. The literal is
// reachable by nothing but the function's own values (no call, store, jump or
// return takes it), its fields are written once, by CreateDataProperty, in
// the block that made it, and a read of one of them cannot reach a prototype
// or run code: the key is an own data property of an object nothing else can
// have touched. A read of any other key (`length`, a method, a key the
// literal does not spell) asks the prototype chain, so it counts as a use
// that holds the literal, and so does everything else.
//
// After inlining, which is what brings a literal and its reads into one
// function, and before the passes that plan shapes and guards over the
// stores and reads this removes. Returns the functions it changed, whose
// reads are now the values stored (boxes, mostly), for the caller to fold.
std::vector<uint32_t> removeUnheldLiterals(il::Module& module);

}  // namespace bronze::lower
