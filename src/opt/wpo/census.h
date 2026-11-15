#pragma once

// The site census: for every property and method site of the module, what
// the whole-program analysis knows about its receivers.

#include <cstdint>
#include <string>
#include <vector>

#include "opt/wpo/shape.h"
#include "opt/wpo/solver.h"

namespace bronze::wpo {

// A named-key property site (`prop.get` / `prop.set`).
enum class PropClass : uint8_t {
    Unreached,      // no receiver reaches it
    Unknown,        // a receiver is unknown (`topObj`)
    Primitive,      // only primitives (a string's `length`)
    SingleShape,    // one runtime shape, an own data slot
    SameSlot,       // several shapes, the key at one slot in all of them
    SameSlotEscaped,// as SameSlot, but a receiver escaped: its shape may grow
    SlotDiffers,    // own in all, at different slots
    NotInPrefix,    // own, but added outside a definite prefix for some receiver
    Inherited,      // found on the prototype chain (a method read, a shared constant)
    Accessor,       // an accessor somewhere on the way
    BuiltinRecv,    // arrays, typed arrays, collections, builtin objects
    FnStatic,       // a function's own property (a static)
    Absent,         // on no receiver or prototype: reads undefined
    Mixed,          // receivers disagree on where the key lives
    // Index-keyed sites (`te[0]`).
    IndexArray,
    IndexTyped,
    IndexOther,
};
const char* propClassName(PropClass c);

enum class CallClass : uint8_t {
    Unreached,
    Unknown,        // the receiver is unknown
    UnknownTarget,  // the receivers are known, the method value is not
    Single,         // one IL function
    Multi,          // several IL functions
    Builtin,        // only builtins
    Mixed,          // builtins and IL functions
    None,           // nothing callable (a TypeError)
};
const char* callClassName(CallClass c);

struct SiteFact {
    uint32_t fn = 0;
    uint32_t block = 0;
    uint32_t inst = 0;
    il::Op op = il::Op::PropGet;
    uint32_t key = 0;
    bool isCall = false;
    PropClass prop = PropClass::Unreached;
    CallClass call = CallClass::Unreached;
    int slot = -1;                 // SingleShape / SameSlot(Escaped)
    size_t receivers = 0;          // abstract receiver objects
    size_t holders = 0;            // Inherited: distinct holder objects
    std::vector<uint32_t> targets; // MethodCall: IL functions
    std::string cause;             // Unknown / UnknownTarget: where the unknown came from
    std::string escapeWhy;         // SameSlotEscaped / UnknownTarget: an escaped receiver's reason
    std::string detail;            // receiver summary for the per-site dump
    bool partial = false;          // unknown receivers beside the known ones
    // Some receiver lacks the key on its whole chain and every other holds it
    // as its own data: the read answers `undefined` or an own slot.
    bool ownOrAbsent = false;
};

std::vector<SiteFact> classifySites(Solver& solver, const ShapeAnalysis& shapes);

}  // namespace bronze::wpo
