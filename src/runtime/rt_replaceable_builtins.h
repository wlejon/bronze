#pragma once

#include <cstddef>

#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze {
struct ObjectHeader;
class PropertyKey;
}  // namespace bronze

// The builtins a page may REPLACE although lockdown froze them: `Math.random`
// and `Date.now`, which a test harness or a recorder swaps for a seeded
// generator and a fixed clock so that an animated scene draws the same frame
// twice. The builtins are the thread's and shared by every realm, so a
// replacement cannot be stored on them.
//
// Each starts as the ordinary data property, read and called at full speed.
// The first assignment to one (`Math.random = f`, `Date.now = g`, from any
// realm) reaches ObjectHeader::setProp's refusal of a non-writable property,
// which asks here first: the property becomes, for the rest of the thread, an
// accessor whose getter answers what the CURRENT realm assigned
// (Realm::replacedBuiltin) or the builtin when it assigned nothing, and whose
// setter stores the value for the current realm alone; the holder moves to a
// fresh shape so no inline cache keeps reading the old slot. Freezing leaves
// an accessor's setter working, so the holder stays frozen and every other
// member stays locked.
//
// What a program can see once one is replaced: its descriptor is
// { get, set, enumerable: false, configurable: false } where the spec gives a
// writable data property; `Object.defineProperty` of the name is still
// refused, as on any frozen builtin.
namespace bronze::runtime {

enum class ReplaceableBuiltin : size_t { MathRandom = 0, DateNow = 1 };

// Records `holder[name]` (already defined, as a data property holding
// `original`) as replaceable. Called once per thread while the holder is
// built, before lockdown.
void rtRegisterReplaceableBuiltin(Value holder, const char* name, ReplaceableBuiltin which,
                                  Value original);

// setProp's refusal of a write to `obj`'s own non-writable `name`: when that is
// a replaceable builtin, converts it (once) and stores `val` for the current
// realm, and answers true; otherwise false and the refusal stands.
bool rtReplaceBuiltinOnWrite(ObjectHeader* obj, PropertyKey name, Value val);

}  // namespace bronze::runtime
