#pragma once

// The site rewrites, one file each, and the small shared pieces they use.
// Internal to the speculation step. Each rewrite takes a plan evidence.cpp
// made for the site and asserts what the plan promises: by the time a
// rewrite runs, refusing is too late (the driver has already made S).

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "il/il.h"
#include "opt/speculate/context.h"
#include "opt/speculate/evidence.h"
#include "opt/speculate/guard_builder.h"

namespace bronze::opt::speculate {

// prop_read.cpp, prop_write.cpp, method_call.cpp, plain_call.cpp, arith.cpp,
// elem.cpp. Each rewrites the instruction at (plan.block, plan.index) of S.
void rewritePropRead(RewriteContext& ctx, const SitePlan& plan);
void rewritePropWrite(RewriteContext& ctx, const SitePlan& plan);
void rewriteMethodCall(RewriteContext& ctx, const SitePlan& plan);
void rewritePlainCall(RewriteContext& ctx, const SitePlan& plan);
void rewriteArith(RewriteContext& ctx, const SitePlan& plan);
void rewriteElem(RewriteContext& ctx, const SitePlan& plan);

// arith.cpp: whether `inst` is an operation the arithmetic rewrite knows, and
// the form a set of operand masks (evidence.h kTagInt / kTagDouble, one per
// operand) allows for it — Int when every operand held only int32 Numbers and
// the operation has an exact int32 form, else Double when every operand held
// only Numbers, else nothing.
bool arithSupported(const il::Instruction& inst);
std::optional<ArithForm> chooseArithForm(const il::Instruction& inst, const std::vector<uint8_t>& operandTags);

// prop_shape.cpp: the shape test every own-slot rewrite starts with.
// Guards that `object` (Dynamic) is a heap object and returns its shape word.
ValueId emitShapeOf(GuardBuilder& b, ValueId object);
// `ptr.eq shape, (shape.cell <cell of key>)`.
ValueId emitShapeIs(GuardBuilder& b, ValueId shape, const std::string& key);
// The two together for one shape: guards `object` is an object of the shape.
void guardShape(GuardBuilder& b, ValueId object, const std::string& key);

// direct_call.cpp: `call @entry(f)` with the convention policy.h
// directCallFits states, at the cursor, where a dominating `fn.code_is
// callee, @entry(f)` holds. `thisValue` is the receiver (`undefined` for a
// plain call), `args` the source arguments the generic site passed. Defines
// the site's result (Dynamic): the call's, boxed or `undefined` as the
// callee's return type needs.
void emitDirectCall(GuardBuilder& b, uint32_t callee, ValueId calleeValue, ValueId thisValue,
                    const std::vector<ValueId>& args);
// `fn.code_is callee, @entry(f)`.
ValueId emitCodeIs(GuardBuilder& b, ValueId calleeValue, uint32_t callee);

// loops.cpp: places a loop-entry resume point at the top of the header of
// every loop of G that holds one of `planned` (sites the rewrite guarded).
// Returns how many it placed.
uint32_t placeLoopEntries(RewriteContext& ctx, const std::vector<SitePlan>& planned);

}  // namespace bronze::opt::speculate
