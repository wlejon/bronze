#pragma once

// WHAT A BOX OF A TYPED VALUE LOOKS LIKE to the tag tests and the unchecked
// unboxes. The one place this pass reads the value encoding from; every rule
// that depends on it asks `boxedForm`.
//
// Two different questions must not be confused:
//   - VALUE semantics: `box.i32 %x` and `box.f64 (i32.to_f64 %x)` are the
//     same JS Number. Every operation defined on JS values (arithmetic,
//     compares, ToBoolean, stores, calls, a `leave` handing the value to G)
//     treats them alike.
//   - ENCODING: what the tag tests and the unchecked unboxes read off the
//     bits. A Number has one box, the double's bits (runtime/value.h), so
//     today the two boxes above are the same word; the table still states
//     each form's answers rather than letting rules assume it.
// `boxedForm` answers the second. The first needs no table, and the rules
// that rely on it (rule 2 replacing one box of a Number by another, see
// params_apply.cpp) state the argument where they use it:
//
//   INTERCHANGEABILITY. `box.i32 %x` and `box.f64 (i32.to_f64 %x)` are one
//   word, so any consumer is correct for either. Inside S the
//   encoding-reading forms are guards and the unbox each guard licenses, and
//   both read the SAME SSA value: the test and the unbox agree about it, and
//   a test that now fails where it passed only sends S to G, which is correct
//   either way.

#include <optional>

#include "il/il.h"

namespace bronze::opt::repr {

struct BoxedForm {
    // The answer of each tag test on `box.<t> %x`, for every %x of type t, or
    // nullopt where it depends on the value.
    std::optional<bool> isI32;
    std::optional<bool> isNumber;
    std::optional<bool> isObject;
    std::optional<bool> isNullish;
    std::optional<bool> isHole;
    // `unbox.f64.unchecked` of the box computes the Number the box holds.
    bool unboxF64ReadsValue = false;
};

// The form of `box.<boxType>`, or nullopt for a type no box of which this
// describes (dynamic, ptr, void).
//
// THE ASSUMED ENCODING (runtime/value.h, the backend's `box` lowering in
// codegen-brass/il2mir/il_lowering_inst.cpp):
//   box.i32  -> the bits of the int32's double (bronze_box_i32 and the
//               backend's `box`): `is.number` and `is.i32` true, and
//               `unbox.f64.unchecked` reads the value. (The Int32 tag of
//               runtime/value.h is no Number's box; nothing makes one.)
//   box.f64  -> the double's own bits, NaN canonicalised: `is.number` true;
//               `is.i32` depends on the value (integral, in range, not -0).
//   box.bool -> the Bool tag; box.str -> the String tag: no number test passes.
// None of them is an object, nullish or the hole.
// If the runtime changes how an int32 is boxed, this function is the one to
// change.
std::optional<BoxedForm> boxedForm(il::Type boxType);

// Whether a double is a Number `is.i32` accepts: integral, within int32 and
// not -0 (il_op.h `is.i32`). NaN and the infinities are not.
bool isInt32Double(double d);

}  // namespace bronze::opt::repr
