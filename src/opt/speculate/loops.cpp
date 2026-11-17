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

uint32_t placeLoopEntries(RewriteContext& ctx, const std::vector<SitePlan>& planned,
                          const std::vector<BlockId>& left) {
    const il::Function& g = ctx.generic;
    const Cfg cfg(g);
    const DomTree dom(cfg);
    const LoopForest loops(cfg, dom);
    uint32_t placed = 0;
    for (const Loop& loop : loops.loops()) {
        // b0 takes no parameters and cannot be given a preheader
        // (cfg_edit.h ensurePreheader), so nothing could resume there.
        if (loop.header == 0 || !mayPlaceResumePoint(g, loop.header)) continue;
        const bool guarded =
            std::any_of(planned.begin(), planned.end(), [&](const SitePlan& p) { return loop.contains(p.block); }) ||
            std::any_of(left.begin(), left.end(), [&](BlockId b) { return loop.contains(b); });
        if (!guarded) continue;
        // A site at the header's top already has a point there, with the
        // same state: it serves the loop entry as well.
        if (!ctx.resume.has(loop.header, 0)) {
            ctx.resume.at(loop.header, 0);
            ++placed;
        }
        if (ctx.optimistic && !ctx.resume.has(loop.header, 0, true)) {
            ctx.resume.at(loop.header, 0, true);
            ++placed;
        }
    }
    return placed;
}

namespace {

// Where each value of S is defined, to test it is available at the end of the
// preheader: a function parameter always, else a definition in the preheader
// itself (every one precedes its terminator) or in a block that dominates it.
class Availability {
public:
    Availability(const il::Function& s, BlockId preheader)
        : defBlock_(s.valueCount, il::kNoBlock), cfg_(s), dom_(cfg_), preheader_(preheader) {
        for (size_t p = 0; p < s.params.size() && p < defBlock_.size(); ++p) defBlock_[p] = kFunctionParam;
        for (const il::Block& block : s.blocks) {
            for (const il::BlockParam& p : block.params) {
                if (p.id < defBlock_.size()) defBlock_[p.id] = block.id;
            }
            for (const il::Instruction& inst : block.instructions) {
                if (inst.result != il::kNoValue && inst.result < defBlock_.size()) defBlock_[inst.result] = block.id;
            }
        }
    }
    bool atPreheaderEnd(ValueId v) const {
        if (v >= defBlock_.size() || defBlock_[v] == il::kNoBlock) return false;
        const BlockId def = defBlock_[v];
        return def == kFunctionParam || def == preheader_ || dom_.dominates(def, preheader_);
    }

private:
    static constexpr BlockId kFunctionParam = il::kNoBlock - 1;
    std::vector<BlockId> defBlock_;
    Cfg cfg_;
    DomTree dom_;
    BlockId preheader_;
};

// The preheader's jump into `header`, when that is how the preheader ends.
const il::Instruction* entryJump(const il::Function& s, BlockId header, BlockId preheader) {
    const auto& preInsts = s.blocks[preheader].instructions;
    if (preInsts.empty()) return nullptr;
    const il::Instruction& entry = preInsts.back();
    if (entry.op != il::Op::Jump || entry.target.block != header ||
        entry.target.args.size() != s.blocks[header].params.size()) {
        return nullptr;
    }
    return &entry;
}

// G's resume point `id` of kind `kind`, or nullptr (the redundancy pass on G
// deletes the points nothing named, and representation selection asks after).
const il::Instruction* findPoint(const il::Function& g, uint32_t id, uint32_t kind) {
    for (const il::Block& block : g.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op == il::Op::ResumePoint && inst.immI32 >= 0 && static_cast<uint32_t>(inst.immI32) == id) {
                return inst.keyIndex == kind ? &inst : nullptr;
            }
        }
    }
    return nullptr;
}

// A loop of an inlined callee (speculate.h): answered from S's
// inlinedLoopEntries.
std::optional<LoopEntryResume> inlinedLoopEntry(const il::Module& module, const il::Function& s, BlockId header,
                                                BlockId preheader, const std::function<ValueId(ValueId)>& current,
                                                bool optimistic) {
    const il::Block& sHeader = s.blocks[header];
    const il::Function::InlinedLoopEntry* found = nullptr;
    for (const il::Function::InlinedLoopEntry& e : s.inlinedLoopEntries) {
        const bool all = std::all_of(sHeader.params.begin(), sHeader.params.end(), [&](const il::BlockParam& p) {
            return std::find(e.headerParams.begin(), e.headerParams.end(), p.id) != e.headerParams.end();
        });
        if (!all) continue;
        const il::Instruction& leave = e.exit.back();
        if (leave.op != il::Op::Leave || leave.calleeIndex != s.speculatedFrom) continue;
        if (leave.keyIndex != (optimistic ? il::kResumeOptimistic : 0u)) continue;
        found = &e;
        break;
    }
    if (found == nullptr) return std::nullopt;
    const il::Instruction* entry = entryJump(s, header, preheader);
    if (entry == nullptr) return std::nullopt;

    const Availability avail(s, preheader);
    const std::vector<il::Type> sTypes = valueTypes(s);
    std::vector<ValueId> own;
    LoopEntryResume out;
    for (const il::Instruction& code : found->exit) {
        il::Instruction inst = code;
        // The types the operands must have: a point's state as its G has
        // them; the conversions take what finishGenerically gave them.
        const il::Instruction* point = nullptr;
        std::vector<il::Type> gTypes;
        if (inst.op == il::Op::ResumeCall || inst.op == il::Op::Leave) {
            if (inst.calleeIndex >= module.functions.size() || inst.immI32 < 0) return std::nullopt;
            const il::Function& g = module.functions[inst.calleeIndex];
            point = findPoint(g, static_cast<uint32_t>(inst.immI32), inst.keyIndex);
            if (point == nullptr || point->operands.size() != inst.operands.size()) return std::nullopt;
            gTypes = valueTypes(g);
        } else if (inst.op != il::Op::Box && inst.op != il::Op::ConstUndefined && inst.op != il::Op::ConstructResult) {
            return std::nullopt;
        }
        for (size_t i = 0; i < inst.operands.size(); ++i) {
            ValueId& v = inst.operands[i];
            if (std::find(own.begin(), own.end(), v) != own.end()) continue;
            ValueId mine = current(v);
            auto it = std::find_if(sHeader.params.begin(), sHeader.params.end(),
                                   [&](const il::BlockParam& p) { return p.id == v; });
            if (it != sHeader.params.end()) mine = entry->target.args[static_cast<size_t>(it - sHeader.params.begin())];
            if (!avail.atPreheaderEnd(mine) || mine >= sTypes.size()) return std::nullopt;
            il::Type want = il::Type::Dynamic;
            if (point != nullptr) {
                const ValueId gv = point->operands[i];
                if (gv >= gTypes.size()) return std::nullopt;
                want = gTypes[gv];
            } else if (inst.op == il::Op::Box) {
                want = inst.boxType;
            }
            if (sTypes[mine] != want) return std::nullopt;
            v = mine;
        }
        if (inst.result != il::kNoValue) own.push_back(inst.result);
        if (inst.op == il::Op::Leave) {
            out.resumeId = static_cast<uint32_t>(inst.immI32);
            out.stateInS = std::move(inst.operands);
        } else {
            out.exitCode.push_back(std::move(inst));
        }
    }
    return out;
}

}  // namespace

std::optional<LoopEntryResume> loopEntryResume(const il::Module& module, const il::Function& s, BlockId header,
                                               BlockId preheader, const std::function<ValueId(ValueId)>& current,
                                               bool optimistic) {
    if (!s.isSpeculated() || s.speculatedFrom >= module.functions.size()) return std::nullopt;
    const il::Function& g = module.functions[s.speculatedFrom];
    if (header >= s.blocks.size() || preheader >= s.blocks.size() || header == preheader) return std::nullopt;
    const il::Block& sHeader = s.blocks[header];
    if (sHeader.params.empty()) return std::nullopt;

    // G's header: the one block whose parameters S's header still has. None:
    // the loop may be an inlined callee's.
    std::vector<BlockId> paramOwner(g.valueCount, il::kNoBlock);
    for (const il::Block& block : g.blocks) {
        for (const il::BlockParam& p : block.params) {
            if (p.id < paramOwner.size()) paramOwner[p.id] = block.id;
        }
    }
    BlockId gHeader = il::kNoBlock;
    for (const il::BlockParam& p : sHeader.params) {
        if (p.id >= paramOwner.size() || paramOwner[p.id] == il::kNoBlock) {
            return inlinedLoopEntry(module, s, header, preheader, current, optimistic);
        }
        if (gHeader == il::kNoBlock) gHeader = paramOwner[p.id];
        if (paramOwner[p.id] != gHeader) return std::nullopt;
    }
    const auto& gInsts = g.blocks[gHeader].instructions;
    // The ordinary point first, then the optimistic one (resume_points.h).
    const size_t at = optimistic ? 1 : 0;
    if (gInsts.size() <= at || gInsts[at].op != il::Op::ResumePoint ||
        gInsts[at].keyIndex != (optimistic ? il::kResumeOptimistic : 0u)) {
        return std::nullopt;
    }
    const il::Instruction& point = gInsts[at];
    const il::Instruction* entry = entryJump(s, header, preheader);
    if (entry == nullptr) return std::nullopt;

    const Availability avail(s, preheader);
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
            if (it != sHeader.params.end()) mine = entry->target.args[static_cast<size_t>(it - sHeader.params.begin())];
        }
        if (!avail.atPreheaderEnd(mine)) return std::nullopt;
        if (mine >= sTypes.size() || v >= gTypes.size() || sTypes[mine] != gTypes[v]) return std::nullopt;
        out.stateInS.push_back(mine);
    }
    return out;
}

}  // namespace bronze::opt::speculate
