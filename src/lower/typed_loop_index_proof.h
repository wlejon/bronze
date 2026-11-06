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
// in `fn`) whose index is `p` or `p + k`, where `p` is a parameter of
// `header` of the counted shape — entered as an integral constant, only ever
// advanced by a non-negative integral constant k <= 2^31.
//
// A TYPED op (`boxedIndex` false) reads its index as the f64 operand, and is
// flagged only where the access is reached through the true edge of
// `p < L` (or `<=`) with L an unboxed entry `length` read in `lengths`, a
// pristine view's length below 2^32: then `p` is a finite safe integer there.
//
// A PROVEN-ARRAY elem.get / elem.set (`boxedIndex` true) reads it through the
// `box.f64` its index operand is, and needs no bound: an integer plus an
// integer is an integer in doubles (every double from 2^53 up is one, and the
// sum rounds to one; a finite sum of a finite value and a constant at most
// 2^31 never reaches infinity), and an integral index past any array — or a
// negative one — is one the unsigned bounds compare sends to the helper with
// the boxed index as it is.
//
// Returns how many ops were flagged.
size_t proveIntegralIndices(il::Function& fn, const Cfg& cfg, const std::vector<DefSite>& defs, il::BlockId header,
                            const std::vector<il::ValueId>& lengths,
                            const std::vector<std::pair<il::BlockId, uint32_t>>& sites, bool boxedIndex);

}  // namespace bronze::lower
