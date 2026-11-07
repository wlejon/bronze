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
// Granted by a `--pins ... numeric-elements` declaration (types/pins.h).
// Nothing PROVES the assumption; enforcement is meant to move to the write paths. Deliberately
// outside the types::TypedArrayElem range so nothing sound can collide with it.
inline constexpr int32_t kElemKindPlainArrayF64 = 100;

// Bit-flags packed into the upper 16 bits of `ElemGetTyped` / `ElemSetTyped`
// `immI32` alongside the low-16-bit element kind (`immI32 & kElemKindMask`):
//   - `kElemFlagProvenIntegralIndex`: the index operand is statically proven
//     to be a finite safe integer, licensing the backend to skip the
//     `sitofp(fptosi(idx)) == idx` round-trip check.
//   - `kElemFlagProvenInBounds`: the index operand is statically proven to
//     satisfy `0 <= idx < view.length` on an unmutated TypedArray view,
//     licensing the backend to skip the unsigned bounds check.
//   - `kElemFlagArrayReceiver` (on `ElemGet` / `ElemSet`): the receiver is
//     an Array, tested by a dominating `is.array` (il_op.h), so the inline
//     path skips its tag and header compares; `kElemFlagProvenIntegralIndex`
//     beside it says the boxed index is an integral Number. On `ElemSet` the
//     flags sit above the write mode (il.h kWriteModeMask).
//   - `kElemFlagFloatViewReceiver` (on `ElemGet` / `ElemSet`): the receiver
//     is a Float32Array or a Float64Array, tested by a dominating
//     `is.typed_array r, kTypedArrayAnyFloat` (typed_body_version.h), so the
//     inline path skips the tag, Array and view header compares and the kind
//     ladder, and asks only which of the two float kinds it is. The access
//     keeps its exact semantics: a Number index outside the view reads
//     `undefined` and drops a Number store inline; an index or value that is
//     no Number takes the helper.
inline constexpr int32_t kElemKindMask = 0xFFFF;
inline constexpr int32_t kElemFlagProvenIntegralIndex = 1 << 16;
inline constexpr int32_t kElemFlagProvenInBounds = 1 << 17;
inline constexpr int32_t kElemFlagArrayReceiver = 1 << 18;
inline constexpr int32_t kElemFlagFloatViewReceiver = 1 << 19;

// `is.typed_array`'s kind selector for "a Float32Array or a Float64Array":
// outside every element kind's number, below kTypedArrayPristineLength (il_op.h),
// which it is never combined with.
inline constexpr int32_t kTypedArrayAnyFloat = 0x7F;

}  // namespace bronze::il
