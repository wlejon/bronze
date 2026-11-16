// A checked read of a binding that nothing writes once it is initialized
// (il.h envImmutable on env.get.tdz: an outer scope's `const`, or a `let` its
// whole reach never assigns). In S it is the plain `env.get` of the slot,
// marked immutable, and a guard that the word read is not the
// uninitialized-binding marker. The read is then killed by nothing
// (opt/redundancy/location.h), so it and its test leave a loop for its entry:
// a module constant read once per call instead of once per iteration, behind
// one test.
//
// Why it is sound: a read that finds the marker leaves for the point in front
// of the site, where G reads the slot again and throws the ReferenceError
// (or, initialized meanwhile, goes on). A read that finds a value finds the
// binding's only value, so wherever the redundancy pass lets one read stand
// for a later one, the later read would have seen the same word.
//
// No evidence is asked for: the guard fails only where G would throw, and
// a leave costs less than the throw it stands in front of.

#include <cassert>

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteTdzRead(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::TdzRead);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(site.op == il::Op::EnvGetTdz && site.envImmutable);
    il::Instruction read;
    read.op = il::Op::EnvGet;
    read.type = site.type;
    read.operands = site.operands;
    read.envDepth = site.envDepth;
    read.envIndex = site.envIndex;
    read.envImmutable = true;
    b.emitResult(std::move(read));
    b.guard(b.emit(il::Op::IsUninit, il::Type::Bool, {site.result}), false);
    b.finish();
    ++ctx.stats.tdzReads;
}

}  // namespace bronze::opt::speculate
