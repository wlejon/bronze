#pragma once

// A method call of a native builtin entered at the site, past the method
// cache's stub and the runtime's generic dispatch: the receiver and the
// arguments handed over in registers to the builtin's fast entry
// (runtime/native_fast.h), or, for an Array member with no fast entry of its
// own, to the builtin's code itself with an argv. Everything the guard does
// not accept takes the site's ordinary call.
//
// Two receiver kinds, and two guards:
//
//   Array   an Array with no named-property box. Its member is the
//           intrinsic's for the reason the method cache's EXOTIC Array entry
//           and il_array_method.h give: the box is what any shadowing gives
//           an array, and Array.prototype refuses every change. The guard is
//           the receiver's header and box; no cache word is read.
//   String  a String, with the site's way 0 the method cache's PRIMITIVE
//           entry (bronze_abi_layout.h): `String.prototype` of the shape the
//           entry latched, and the function in its slot NOW a function object
//           whose code is this builtin's. A program may write
//           `String.prototype.charCodeAt = f` without moving the shape, which
//           is why the slot is re-read and its code compared at every call.
//
// The fast entries re-test the receiver's kind and their arguments and hand
// the builtin anything else, so an arm's guard decides only which code is
// entered for it, never an answer.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>
#include <vector>

namespace il2mir {

class IlLowering;

struct NativeFastRow;

// The rows this method call takes, in the order their arms are tested (an
// Array's `indexOf` and a String's share a key): a key and argument count the
// table names, in code that runs more than once, under a pipeline whose
// method sites test the cache at the site. A String row needs the builtin's
// code address, which only an in-process tier or an image (whose loader binds
// the import to the function) has.
std::vector<const NativeFastRow*> native_fast_rows(IlLowering* lowering, const BronzeInstruction& inst);

// The rows' arms in front of `slow` (the site's ordinary call), with the
// site's arguments boxed. Returns the call's result as Value bits.
Value* lower_native_fast(IlLowering* lowering, Builder& b, const std::vector<const NativeFastRow*>& rows, Value* recv,
                         const std::vector<Value*>& boxed_args, Value* site,
                         const std::function<Value*(Builder&)>& slow);

}  // namespace il2mir
