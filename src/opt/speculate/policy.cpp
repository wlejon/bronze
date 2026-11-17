#include "opt/speculate/policy.h"

#include "abi/bronze_abi.h"
// The backend's rule, header-only over il and abi: where its resume twin can
// start from a point's state alone. Read here rather than restated, so the
// two cannot drift.
#include "il/resume_rules.h"
#include "opt/speculate/speculate.h"

namespace bronze::opt::speculate {

bool mayPlaceResumePoint(const il::Function& fn, BlockId block) {
    // A G with resume points is the function a leave lands in, so this is
    // asked of G as lowered (it is never speculated itself).
    if (block >= fn.blocks.size()) return false;
    return il::canResumeIn(fn) && il::canResumeAt(fn, fn.blocks[block]);
}

bool functionMaySpeculate(const il::Function& fn) {
    return !fn.isExternal && !fn.isSpeculated() && !fn.isEntryPoint && !fn.isCoroutineBody() &&
           !fn.blocks.empty();
}

namespace {

bool conventionFits(const il::Module& module, const il::Function& callee, size_t argc) {
    if (!il::functionEntersDirectly(module, callee)) return false;
    if (callee.needsArguments || callee.hasRestParam) return false;
    if (argc > 16) return false;
    for (size_t p = callee.firstSourceParam(); p < callee.params.size(); ++p) {
        if (callee.params[p].type != il::Type::Dynamic || callee.params[p].pinned) return false;
    }
    // The synthetic leading parameters are Dynamic by construction; a callee
    // whose are not was made by something this convention does not know.
    for (size_t p = 0; p < callee.firstSourceParam(); ++p) {
        if (callee.params[p].type != il::Type::Dynamic) return false;
    }
    return true;
}

}  // namespace

bool directCallFits(const il::Module& module, const il::Function& callee, size_t argc) {
    return (callee.fnFlags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) == 0 && conventionFits(module, callee, argc);
}

bool constructFits(const il::Module& module, const il::Function& callee, size_t argc) {
    return callee.needsThis && conventionFits(module, callee, argc);
}

}  // namespace bronze::opt::speculate
