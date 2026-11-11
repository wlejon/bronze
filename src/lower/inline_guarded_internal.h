#pragma once

// Shared state between guarded inlining's target selection (`inline_guarded.cpp`)
// and CFG splice emission (`inline_guarded_emit.cpp`).

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lower/inline_guarded.h"

namespace bronze::lower::inline_guarded_detail {

// The sizes below are in IL instructions of FINISHED bodies (guarded regions
// and typed entries already built), and were set on the three.js benches
// (docs/il2mir.md section 10 has the measurements).
//
// A callee's size: the body a site pays for in code and build time on top of
// the call it keeps for the slow path. `Matrix4.compose` (209) is inside it;
// leaving it out while its caller `Object3D.updateMatrix` came in measured
// slower than inlining neither, since the call left behind sat in a loop body
// that now held every other value live across it.
inline constexpr size_t kMaxCalleeInstructions = 250;
// Tiny getters, setters, and leaf vector/point helpers (`<= 32` instructions)
// cost very little code and eliminate hot IC/call overhead inside loops.
inline constexpr size_t kTinyCalleeInstructions = 32;
// Inline frames deep: a body inlined into a body inlined into the caller.
// pixi's `sprite.x = v` is four (`Container.set x`, `ObservablePoint.set x`,
// `_onUpdate`, `RenderGroup.onChildUpdate`).
inline constexpr size_t kMaxDepth = 5;
// A non-loop caller bigger than `kMaxCallerStart` takes no bodies: what is big
// already without loops is mostly cold library code (parsers, loaders). Callers
// with loops (often already versioned by `guardRegion` / `typedLoopVersion`)
// or entered from loops admit up to `kMaxHotCallerStart`.
inline constexpr size_t kMaxCallerStart = 800;
inline constexpr size_t kMaxHotCallerStart = 2400;
// A caller grows to at most twice its size plus this, and never past the cap.
inline constexpr size_t kCallerGrowthSlack = 200;
// ...or by this much, whichever is more: room for one callee of the largest
// size with its blocks' joins, so a small method's one big call (`updateMatrix`
// calling `compose`) is not the one call it can never take.
inline constexpr size_t kMinCallerGrowth = 320;
inline constexpr size_t kMinHotCallerGrowth = 1600;
inline constexpr size_t kMaxCallerInstructions = 1500;
inline constexpr size_t kMaxHotCallerInstructions = 3600;
// The fixed-argument method.call form's widest list (il/verifier.cpp).
inline constexpr size_t kMaxArgs = 16;

// What the closure guard adds to B and E beyond a method guard's two; a
// chained closure arm reuses B's `method.slot.fn` result and adds one fewer.
inline constexpr size_t kClosureGuardInstructions = 3;
inline constexpr size_t kChainedClosureGuardInstructions = 2;

// How a body is entered (Inliner::inlineAt).
enum class Form { Method, Closure, Exact, Literal };

struct SplicePlan {
    Form form = Form::Method;
    uint32_t nextTry = 0;       // next index in the site's chained target list
    bool moreTries = false;     // whether `slow` stays open for remaining targets
    size_t reserveSlow = 0;     // caller room held back until `slow` is visited
};

bool trace();
size_t instructionCount(const il::Function& fn);
std::vector<il::Type> valueTypes(const il::Function& fn);
std::vector<uint8_t> loopBlocks(const il::Function& fn);
bool isCandidate(const il::Function& fn, bool closure, bool exact = false);

class Inliner {
public:
    Inliner(il::Module& module, const ClosureGuesses* guesses, const ColdBlocks* cold,
            const ClosureGuesses* polyMethods, const ClosureGuesses* polyOwnCallees);

    // Inlines every eligible call in `fnIdx`, its copied bodies' calls
    // included, within the depth and size bounds. When `tinyOnly` is set, only
    // callees up to `kTinyCalleeInstructions` are considered. Returns how many.
    size_t inlineInto(uint32_t fnIdx, std::unordered_set<uint32_t>* inlinedOwnIcs,
                      bool tinyOnly = false);

private:
    static constexpr uint32_t kNone = UINT32_MAX;

    uint32_t unguardedCallee(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst, Form& form);
    void noteLiteral(const il::Instruction& inst);
    bool eligibleMethodTarget(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst,
                              uint32_t origin, uint32_t callee) const;
    bool eligibleClosureTarget(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst,
                               uint32_t target) const;
    uint32_t eligibleCallee(const il::Function& fn, uint32_t fnIdx, const il::Instruction& inst) const;
    uint32_t polyMethodCallee(const il::Function& fn, uint32_t fnIdx, il::BlockId b, const il::Instruction& inst,
                              size_t available, SplicePlan& splice, size_t& grows) const;
    uint32_t polyClosureCallee(const il::Function& fn, uint32_t fnIdx, il::BlockId b, size_t instIdx,
                               const il::Instruction& inst, size_t available, SplicePlan& splice, size_t& grows) const;
    uint32_t closureCallee(const il::Function& fn, uint32_t fnIdx, il::BlockId b, const il::Instruction& inst,
                           uint32_t& next) const;

    size_t growthOf(uint32_t callee, size_t extra) const;
    void markLoops(const il::Function& fn) { inLoop_ = loopBlocks(fn); }
    void markEnteredFromLoops();
    uint32_t originOf(uint32_t f) const;
    il::ValueId fresh(il::Function& fn, il::Type type);
    void inlineAt(il::Function& fn, il::BlockId b, size_t i, uint32_t calleeIdx, const SplicePlan& splice);

    il::Module& module_;
    const ClosureGuesses* guesses_ = nullptr;
    const ClosureGuesses* polyMethods_ = nullptr;
    const ClosureGuesses* polyOwnCallees_ = nullptr;
    std::vector<il::Function> originals_;
    // The blocks holding a guard's final fallback call, whose call stays a call.
    std::vector<uint8_t> slowBlock_;
    std::vector<uint8_t> lateBlock_;
    const ColdBlocks* cold_ = nullptr;
    std::vector<uint8_t> inLoop_;
    std::vector<uint8_t> candidate_;
    std::vector<uint8_t> closureCandidate_;
    std::vector<uint8_t> exactCandidate_;
    std::vector<uint8_t> literalCandidate_;
    std::vector<uint8_t> calleeHasLoop_;
    std::unordered_map<il::ValueId, std::pair<uint32_t, il::ValueId>> literals_;
    // Per block: how many of its first call's closure or method targets an
    // earlier guard already took (the miss block of target k starts at k+1).
    std::vector<uint32_t> closureTried_;
    std::vector<uint32_t> methodTried_;
    // Per miss block of a closure guard: the `method.slot.fn` result already
    // loaded in the dominating block, reused by chained `closure.is` tests.
    std::vector<il::ValueId> closureSlotFn_;
    std::vector<size_t> reservedForBlock_;
    std::vector<uint8_t> enteredFromLoop_;
    std::vector<il::Type> types_;
};

}  // namespace bronze::lower::inline_guarded_detail
