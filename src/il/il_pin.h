#pragma once

// The PIN CLAIMS as the IL carries them: what a `--pins` manifest declaration
// (src/types/pins.h) has become by the time it reaches the backend.
//
// One header rather than two scattered declarations in il.h, because these two
// are one thing said about two positions. A manifest entry makes a claim about
// a SLOT and, for `numeric-elements`, a second claim about that slot's
// ELEMENTS; `PinBarrier` names what a guard on the first tests, and
// `kElemKindPlainArrayF64` names what the second turns the element form into.
// Reading either without the other leaves the pair's division of labour
// invisible — which is the argument for the seam, and the reason it is drawn
// here rather than at whatever line count would have been convenient.
//
// Nothing about a manifest survives to this point. What the backend receives
// is a SHAPE to check and an element kind to emit, never the declaration that
// asked for them, which is why these live in `il` and not in `types`.

#include <cstdint>

namespace bronze::il {

// What one `Op::PinGuard` tests, carried in the instruction's `immI32`.
//
// One enum per PIN KIND that has a read-side claim a write can contradict
// (src/types/pins.h). It lives here rather than in `types::PinKind` because
// the backend emits the test and the backend does not know about manifests:
// what reaches it is a SHAPE to check, not the declaration that asked for it.
enum class PinBarrier : uint8_t {
    // `<class>.<field>: number`, `function <fn>.<binding>: number`,
    // `param <owner>(<p>): number`, `return <owner>: number`, and the value
    // of a `numeric-elements` ELEMENT store. The read spends this claim on a
    // raw unbox, so a violation is a pointer's bits read as a double.
    Number,
    // `<class>.<field>: number-or-nullish`. The read stays boxed; what the
    // claim licenses is the branchless coercion, so a violation is a wrong
    // ToNumber rather than a wrong read.
    NumberOrNullish,
    // The FIELD half of `<class>.<field>: numeric-elements`: the slot holds a
    // plain JS Array. The element half is `Number`, checked at each element
    // store; see src/types/pins.h for what this pair does and does not reach.
    DenseArray,
};

// The PINNED element kind: a plain dense JS array whose reads and writes are
// ASSUMED in-bounds, hole-free and numeric — no guard of any kind is emitted.
// Granted by a `--pins ... numeric-elements` declaration (types/pins.h), or by
// the blanket `BRONZE_UNSOUND_PINS` measurement mode. Nothing PROVES the
// assumption; enforcement is meant to move to the write paths. Deliberately
// outside the types::TypedArrayElem range so nothing sound can collide with it.
inline constexpr int32_t kElemKindPlainArrayF64 = 100;

}  // namespace bronze::il
