#pragma once

// The inliner's pieces, internal to it: which callees and call sites qualify
// (callee.cpp), the resume points after the caller's generic calls
// (callee.cpp), and the splice of one body into one call site (splice.cpp).
// driver.cpp orders and chooses. inline.h has the design.

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "il/il.h"

namespace bronze::opt::inliner {

using il::BlockId;
using il::kNoBlock;
using il::kNoValue;
using il::ValueId;

// ---- callee.cpp ------------------------------------------------------------

// A block that ends in `leave`: an exit of a speculated function, cold, run
// only when a guard failed.
bool isExitBlock(const il::Block& block);

// Whether `fn` has a `leave` anywhere (a speculated function whose guards
// remained, or that inlined one whose guards did).
bool hasLeaves(const il::Function& fn);

// THE SIZE the policy counts: instructions, terminators included, outside
// exit blocks. An exit's instructions run only on the way out, so they cost
// code size but nothing on the path the inlining is for.
uint32_t bodySize(const il::Function& fn);

// Whether `fn` may ever have its body copied into a caller. The rules, each
// with its reason, are in callee.cpp.
bool calleeInlinable(const il::Module& module, const il::Function& fn);

// THE POINT AFTER A GENERIC CALL of the caller's G (il.h `resumeAfter`), as
// the inliner needs it: the id, the state in G's ids, and which state value is
// the generic call's own result, whose place the finished call's value takes.
struct AfterCall {
    uint32_t resumeId = 0;
    std::vector<ValueId> state;
    // The generic call's result when the state carries it (it is used after
    // the call), else kNoValue; and its type in G.
    ValueId callResult = kNoValue;
    il::Type callResultType = il::Type::Void;
    // The generic site is a `construct`, whose value is what the constructor
    // returned when that is an object and its instance otherwise.
    bool construct = false;
};

// Every resume point of a generic function G that stands right after a call,
// by id. The speculation step put each `resumeAfter` point in front of the
// instruction after the generic call, and G is unchanged since (the inliner
// runs before any pass edits G), so the instruction just before such a point
// IS the generic call: `call.dynamic`, `method.call`, lowering's direct
// `call`, or the `prop.get` / `prop.set` a getter or setter call stands for
// (resume_points.h places points back to front, and one location has one
// point, so nothing else is inserted between the two).
class AfterCallPoints {
public:
    explicit AfterCallPoints(const il::Function& generic);
    // The point `id` when it stands right after a call, else nullptr.
    const AfterCall* find(uint32_t id) const;

private:
    std::map<uint32_t, AfterCall> points_;
};

// Whether a value of G's type `inState` (the generic call's result) can be
// made from what the callee's G returns, `returned`: the value itself when
// the types agree, and for a Dynamic result `undefined` for a void return or
// a box of a typed one — the conversions emitDirectCall
// (opt/speculate/direct_call.cpp) makes on the fast path for the same call.
bool resultMakeable(il::Type inState, il::Type returned);

// Whether the direct `call` instruction `call` can take `callee`'s body,
// given the caller's point after it (`after`, nullptr when the call has none
// or it names no after-call point). See callee.cpp.
bool siteFits(const il::Instruction& call, const il::Function& callee, const AfterCall* after);

// ---- splice.cpp ------------------------------------------------------------

struct SpliceResult {
    uint32_t leavesRewritten = 0;
    uint32_t instructionsAdded = 0;
};

// Replaces the direct call at instruction `index` of block `block` of
// `module.functions[callerIndex]` (a speculated function) with a copy of
// `callee`'s blocks, which siteFits accepted for it. `callee` is a function of
// the module or, for a self-call, a snapshot of the caller taken before any
// splice into it. Appends blocks to the caller and leaves the indices of
// every other instruction in front of `index` in `block`, and in every other
// existing block, where they were.
SpliceResult spliceCall(il::Module& module, uint32_t callerIndex, BlockId block, size_t index,
                        const il::Function& callee, const AfterCall* after);

}  // namespace bronze::opt::inliner
