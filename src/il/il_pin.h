#pragma once

// The PIN CLAIMS as the IL carries them: what a `--pins` manifest declaration
// (src/types/pins.h) has become by the time it reaches the backend.
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

// Element flags an `ElemGet` / `ElemSet` carries in `immI32` (on `ElemSet`
// above the write mode, il.h kWriteModeMask):
//   - `kElemFlagProvenIntegralIndex`: the boxed index is an integral Number.
//   - `kElemFlagArrayReceiver`: the receiver is an Array, tested by a
//     dominating `is.plain_array` (lower_iter_loop.cpp), so the inline path
//     skips its tag and header compares.
//   - `kElemFlagWordViewReceiver`: the receiver is a WORD VIEW, an
//     Int32Array, Uint32Array, Float32Array or Float64Array, tested by a
//     dominating `is.typed_array` (lower_pattern.cpp), so the inline path
//     skips the tag, Array and view header compares and the full kind
//     ladder, and asks only which of the four kinds it is. The access keeps
//     its exact semantics: a Number index outside the view reads `undefined`;
//     an index or value that is no Number takes the helper.
inline constexpr int32_t kElemFlagProvenIntegralIndex = 1 << 16;
inline constexpr int32_t kElemFlagArrayReceiver = 1 << 18;
inline constexpr int32_t kElemFlagWordViewReceiver = 1 << 19;

// `prop.get r, "length"` `immI32` (unused otherwise by a `prop.get`):
// `r` is an Array, tested by a dominating `is.plain_array`
// (lower_iter_loop.cpp), so the read is the header's length and nothing
// else: an Array's `length` is its own non-configurable data property, which
// no prototype, getter or proxy can answer in its place (10.4.2), and the
// heap kind is fixed for the object's life. The backend loads it as the
// Number it is, with no receiver test and no other arm.
inline constexpr int32_t kPropGetArrayLength = 1;

}  // namespace bronze::il
