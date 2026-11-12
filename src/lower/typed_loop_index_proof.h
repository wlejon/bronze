#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "il/il.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

// The argument `pred` passes for parameter `k` of `to`, or kNoValue when it
// reaches `to` some other way (a handler) or on both edges differently.
il::ValueId incomingArg(const il::Function& fn, il::BlockId pred, il::BlockId to, size_t k);
// The one value every predecessor of `b` passes for its parameter `v`, or
// kNoValue.
il::ValueId soleIncoming(const il::Function& fn, const Cfg& cfg, il::BlockId b, il::ValueId v);

// INTEGRAL INDICES for the typed-loop versioning pass (typed_loop_version.h):
// flags kElemFlagProvenIntegralIndex on the element ops at `sites` (positions
// in `fn`), by what their index is made of. A COUNTER is a parameter `p` of
// `header` entered as an integral constant and only ever advanced by
// non-negative integral constants k <= 2^31.
//
// A TYPED op (`boxedIndex` false) reads its index as the f64 operand, and is
// flagged where that is `p` or `p + k` and the access is reached through the
// true edge of `p < L` (or `<=`) with L an unboxed entry `length` read in
// `lengths`, a pristine view's length below 2^32: then `p` is a finite safe
// integer there.
//
// A PROVEN-ARRAY or word-view elem.get / elem.set (`boxedIndex` true) reads
// it through the `box.f64` its index operand is, and needs no bound: an
// integral index past any array, or a negative one, is one the unsigned
// bounds compare sends to the helper with the boxed index as it is. So its
// index may be any sum, difference or constant multiple of integers:
// `(i + 1) * 2 + 1`, `i + offset`. An integer plus, minus or times an integer
// is an integer in doubles (every double from 2^53 up is one, and the result
// rounds to one), and a few such steps over values that start below 2^53 and
// grow by at most 2^31 an iteration never reach infinity, so never NaN.
//
// `onEntry`, when given, widens what counts as an integer to f64 values
// defined ahead of the loop: a counter's start that is no constant
// (`let i = offset`, `index++` on a parameter) and a loop-invariant operand.
// Each one the proof leaned on is appended, and the flags hold only if the
// copy is entered behind `guardIntegralOnEntry` of them.
//
// Returns how many ops were flagged.
size_t proveIntegralIndices(il::Function& fn, const Cfg& cfg, const std::vector<DefSite>& defs, il::BlockId header,
                            const std::vector<il::ValueId>& lengths,
                            const std::vector<std::pair<il::BlockId, uint32_t>>& sites, bool boxedIndex,
                            std::vector<il::ValueId>* onEntry = nullptr);

// Puts a copy's entry behind a test of each of `values` (f64): an integer of
// magnitude at most 2^53. `entry` is the copy's entry block, ending in the
// jump to its header; a value that fails goes to `miss`, where the entry
// tests that failed before it went.
void guardIntegralOnEntry(il::Function& fn, il::BlockId entry, const il::BlockTarget& miss,
                          const std::vector<il::ValueId>& values);

}  // namespace bronze::lower
