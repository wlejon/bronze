// LOOP-ENTRY RESUME POINTS (speculate.h loopEntryResume states the contract).
//
// The redundancy pass may execute a loop-invariant guard of S once, at the
// end of the loop's preheader, instead of on every iteration. If it fails
// there, S has run exactly what G would have run up to entering the loop, and
// the right place to continue is G's copy of the loop's header, before its
// first instruction, with the header's parameters holding their ENTRY values
// — the arguments of S's preheader jump — and every other live value as S
// has it. So for each loop of G that holds a guarded site this places a
// resume point at the top of the header, whose state `liveBefore(G, H, 0)`
// is the header's live parameters and the values from before the loop that
// the loop or what follows needs.
//
// Placement happens with the sites, from G's own loop forest, before any
// point goes in. The query runs later, on S as passes have left it, so it
// identifies the loop from the IL alone: S keeps G's value ids, and a block
// parameter is never moved to another block, so the ids of S's header
// parameters name G's header. A value the redundancy pass replaced by an
// equal one is followed to its replacement; a value gone otherwise or of
// another type than G's answers nullopt, which keeps the guard in the loop.

#include <algorithm>

#include "opt/core/cfg.h"
#include "opt/core/loops.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

uint32_t placeLoopEntries(RewriteContext& ctx, const std::vector<SitePlan>& planned) {
    const il::Function& g = ctx.generic;
    const Cfg cfg(g);
    const DomTree dom(cfg);
    const LoopForest loops(cfg, dom);
    uint32_t placed = 0;
    for (const Loop& loop : loops.loops()) {
        // b0 takes no parameters and cannot be given a preheader
        // (cfg_edit.h ensurePreheader), so nothing could resume there.
        if (loop.header == 0 || !mayPlaceResumePoint(g, loop.header)) continue;
        const bool guarded = std::any_of(planned.begin(), planned.end(),
                                         [&](const SitePlan& p) { return loop.contains(p.block); });
        if (!guarded) continue;
        // A site at the header's top already has a point there, with the
        // same state: it serves the loop entry as well.
        if (ctx.resume.has(loop.header, 0)) continue;
        ctx.resume.at(loop.header, 0);
        ++placed;
    }
    return placed;
}

std::optional<LoopEntryResume> loopEntryResume(const il::Module& module, const il::Function& s, BlockId header,
                                               BlockId preheader, const std::function<ValueId(ValueId)>& current) {
    if (!s.isSpeculated() || s.speculatedFrom >= module.functions.size()) return std::nullopt;
    const il::Function& g = module.functions[s.speculatedFrom];
    if (header >= s.blocks.size() || preheader >= s.blocks.size() || header == preheader) return std::nullopt;
    const il::Block& sHeader = s.blocks[header];
    if (sHeader.params.empty()) return std::nullopt;

    // G's header: the one block whose parameters S's header still has.
    std::vector<BlockId> paramOwner(g.valueCount, il::kNoBlock);
    for (const il::Block& block : g.blocks) {
        for (const il::BlockParam& p : block.params) {
            if (p.id < paramOwner.size()) paramOwner[p.id] = block.id;
        }
    }
    BlockId gHeader = il::kNoBlock;
    for (const il::BlockParam& p : sHeader.params) {
        if (p.id >= paramOwner.size() || paramOwner[p.id] == il::kNoBlock) return std::nullopt;
        if (gHeader == il::kNoBlock) gHeader = paramOwner[p.id];
        if (paramOwner[p.id] != gHeader) return std::nullopt;
    }
    const auto& gInsts = g.blocks[gHeader].instructions;
    if (gInsts.empty() || gInsts.front().op != il::Op::ResumePoint) return std::nullopt;
    const il::Instruction& point = gInsts.front();

    const auto& preInsts = s.blocks[preheader].instructions;
    if (preInsts.empty()) return std::nullopt;
    const il::Instruction& entry = preInsts.back();
    if (entry.op != il::Op::Jump || entry.target.block != header ||
        entry.target.args.size() != sHeader.params.size()) {
        return std::nullopt;
    }

    // Where each value of S is defined, to test it is available at the end
    // of the preheader: a function parameter always, else a definition in
    // the preheader itself (every one precedes its terminator) or in a block
    // that dominates it.
    std::vector<BlockId> defBlock(s.valueCount, il::kNoBlock);
    constexpr BlockId kFunctionParam = il::kNoBlock - 1;
    for (size_t p = 0; p < s.params.size() && p < defBlock.size(); ++p) defBlock[p] = kFunctionParam;
    for (const il::Block& block : s.blocks) {
        for (const il::BlockParam& p : block.params) {
            if (p.id < defBlock.size()) defBlock[p.id] = block.id;
        }
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result != il::kNoValue && inst.result < defBlock.size()) defBlock[inst.result] = block.id;
        }
    }
    const Cfg sCfg(s);
    const DomTree sDom(sCfg);
    const std::vector<il::Type> sTypes = valueTypes(s);
    const std::vector<il::Type> gTypes = valueTypes(g);

    LoopEntryResume out;
    out.resumeId = static_cast<uint32_t>(point.immI32);
    for (const ValueId v : point.operands) {
        // A header parameter S still has: its value on entry is what the
        // preheader passes it. Anything else, a header parameter a pass found
        // trivial included (every edge passed it one value), is the value
        // standing for it now.
        ValueId mine = current(v);
        if (v < paramOwner.size() && paramOwner[v] == gHeader) {
            auto it = std::find_if(sHeader.params.begin(), sHeader.params.end(),
                                   [&](const il::BlockParam& p) { return p.id == v; });
            if (it != sHeader.params.end()) mine = entry.target.args[static_cast<size_t>(it - sHeader.params.begin())];
        }
        if (mine >= defBlock.size() || defBlock[mine] == il::kNoBlock) return std::nullopt;
        const BlockId def = defBlock[mine];
        if (def != kFunctionParam && def != preheader && !sDom.dominates(def, preheader)) return std::nullopt;
        if (mine >= sTypes.size() || v >= gTypes.size() || sTypes[mine] != gTypes[v]) return std::nullopt;
        out.stateInS.push_back(mine);
    }
    return out;
}

}  // namespace bronze::opt::speculate
