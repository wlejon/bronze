#pragma once

// The site rewrites, one file each, and the small shared pieces they use.
// Internal to the speculation step. Each rewrite takes a plan evidence.cpp
// made for the site and asserts what the plan promises: by the time a
// rewrite runs, refusing is too late (the driver has already made S).

#include <cassert>
#include <cstdint>
#include <functional>
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
void rewritePropAdd(RewriteContext& ctx, const SitePlan& plan);    // prop_write.cpp
void rewritePropSlot(RewriteContext& ctx, const SitePlan& plan);   // prop_slot.cpp
void rewriteConstruct(RewriteContext& ctx, const SitePlan& plan);  // construct.cpp
void rewriteMethodCall(RewriteContext& ctx, const SitePlan& plan);
void rewritePlainCall(RewriteContext& ctx, const SitePlan& plan);
void rewriteArith(RewriteContext& ctx, const SitePlan& plan);
void rewriteElem(RewriteContext& ctx, const SitePlan& plan);
void rewriteArrayLength(RewriteContext& ctx, const SitePlan& plan);  // elem.cpp
void rewriteMathCall(RewriteContext& ctx, const SitePlan& plan);     // math_call.cpp
void rewriteMember(RewriteContext& ctx, const SitePlan& plan);       // member_call.cpp

// arith.cpp: whether `inst` is an operation the arithmetic rewrite knows;
// whether its generic lowering marks the numeric arms (evidence.h
// kArithArm*); and the form the operand masks (evidence.h kTagInt /
// kTagDouble, one per operand) and the result's evidence choose for it
// (arith.cpp says which and why), or nothing.
bool arithSupported(const il::Instruction& inst);
bool numericArms(const il::Instruction& inst);
std::optional<ArithForm> chooseArithForm(const il::Instruction& inst, const std::vector<uint8_t>& operandTags,
                                         bool resultDouble);

// int_sources.cpp: turns the Int-form arithmetic plans of `fn` whose Dynamic
// operands are not produced by int-form operations of the function into
// Double-form ones (the file says why).
void demoteUnsourcedInts(const il::Function& fn, FunctionPlan& plan);

// prop_shape.cpp: the shape test every own-slot rewrite starts with.
// Guards that `object` (Dynamic) is a heap object and returns its shape word.
ValueId emitShapeOf(GuardBuilder& b, ValueId object);
// `ptr.eq shape, (shape.cell <cell of key>)`.
ValueId emitShapeIs(GuardBuilder& b, ValueId shape, const std::string& key);
// The two together for one shape: guards `object` is an object of the shape.
void guardShape(GuardBuilder& b, ValueId object, const std::string& key);
// `shape.in shape, (shape.cell <cell of k1>), ...` for two or more keys,
// emitShapeIs for one.
ValueId emitShapeIn(GuardBuilder& b, ValueId shape, const std::vector<std::string>& keys);
// The own-slot access of a PropRead or PropWrite plan on several shapes (or
// a set's: shape_sets.cpp), the site's result defined: `access(slot)` emits
// the access of one slot at the cursor and returns its value (kNoValue for a
// write). The forms are in prop_shape.cpp.
void emitSlotDispatch(GuardBuilder& b, const SitePlan& plan, ValueId object,
                      const std::function<ValueId(int32_t)>& access);

// direct_call.cpp: `call @entry(f)` with the convention policy.h
// directCallFits states, at the cursor, where a dominating `fn.code_is
// callee, @entry(f)` holds. `thisValue` is the receiver (`undefined` for a
// plain call), `args` the source arguments the generic site passed. Defines
// the site's result (Dynamic): the call's, boxed or `undefined` as the
// callee's return type needs.
void emitDirectCall(GuardBuilder& b, uint32_t callee, ValueId calleeValue, ValueId thisValue,
                    const std::vector<ValueId>& args);
// The same call, its value (Dynamic) returned rather than made the site's.
ValueId emitDirectCallValue(GuardBuilder& b, uint32_t callee, ValueId calleeValue, ValueId thisValue,
                            const std::vector<ValueId>& args);
// `fn.code_is callee, @entry(f)`.
ValueId emitCodeIs(GuardBuilder& b, ValueId calleeValue, uint32_t callee);

// loops.cpp: places a loop-entry resume point at the top of the header of
// every loop of G that holds one of `planned` (sites the rewrite guarded).
// Returns how many it placed.
uint32_t placeLoopEntries(RewriteContext& ctx, const std::vector<SitePlan>& planned);

}  // namespace bronze::opt::speculate
