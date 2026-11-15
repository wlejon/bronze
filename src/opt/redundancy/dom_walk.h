#pragma once

// The dominator-tree walk jobs 1-3 share: opt::walkWithFacts, extended to keep
// an ExprTable in step with the FactEnv.
//
// On entry to block `b` (after pushing a scope on both):
//   - one incoming edge, a branch arm: the edge's facts are added;
//   - a JOIN (several incoming edges, a loop header among them): whatever any
//     path from b's immediate dominator to b writes (regionWrites) kills the
//     facts and the table entries that depended on it. At a loop header that
//     is the loop body's writes, so what survives is exactly what is
//     available on every iteration;
//   - an incoming THROW edge from b's own immediate dominator: the table is
//     hidden (expr_table.h). The handler is entered from the middle of that
//     block, so the values it computed after the throwing instruction may not
//     exist. FactEnv needs no such care: its facts are only added at block
//     entry, so every fact of the dominator held throughout it.
// Then `visit(b)` walks b's instructions itself, querying and updating both,
// and calling `facts.stepOver(inst)` and the table's kills as it goes.
//
// `visit` may rewrite instructions IN PLACE (operands, an op turned into a
// constant with the same result) but must not insert, delete or reorder
// instructions or edit the CFG: the Cfg and DefUse snapshots the walk and
// edge_facts read hold block and instruction indices.

#include <functional>

#include "opt/core/cfg.h"
#include "opt/core/fact_env.h"
#include "opt/core/uses.h"
#include "opt/redundancy/expr_table.h"
#include "opt/redundancy/location.h"

namespace bronze::opt::redundancy {

void walkDominatorTree(const il::Function& fn, const Cfg& cfg, const DomTree& dom, const DefUse& defs,
                       const AliasInfo& alias, FactEnv& facts, ExprTable& exprs,
                       const std::function<void(BlockId)>& visit);

}  // namespace bronze::opt::redundancy
