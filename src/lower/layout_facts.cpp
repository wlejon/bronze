// LAYOUT FACTS: the decision and the rewrite. `layout_facts.h` states the rule
// and why it is sound; this file finds the anchors, places the facts and
// hands them to the sites.

#include "lower/layout_facts.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// Dominance over a graph that splits every block in two: `in(b)` is the
// block's start and `out(b)` its end, `in(b) -> out(b)` is running it, a
// terminator's edge leaves from `out(b)`, and a HANDLER edge leaves from
// `in(b)`, since a protected block may throw before its first instruction
// has run. "`out(A)` dominates `in(T)`" then says every path into T has run
// all of A, which is what a value defined in the middle of A needs to be
// readable in T. (`Cfg` counts a handler as an edge from the block as a whole,
// which is the safe direction for a whole-block question and not for this.)
struct SplitDom {
    std::vector<std::vector<uint32_t>> preds;
    std::vector<uint32_t> idom;
    std::vector<uint32_t> rpoIndex;

    static uint32_t in(il::BlockId b) { return 2 * b; }
    static uint32_t out(il::BlockId b) { return 2 * b + 1; }

    explicit SplitDom(const il::Function& fn) {
        const size_t n = fn.blocks.size() * 2;
        std::vector<std::vector<uint32_t>> succs(n);
        preds.resize(n);
        auto edge = [&](uint32_t from, uint32_t to) {
            if (to >= n) return;
            if (std::find(succs[from].begin(), succs[from].end(), to) != succs[from].end()) return;
            succs[from].push_back(to);
            preds[to].push_back(from);
        };
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            const il::Block& block = fn.blocks[b];
            const auto id = static_cast<il::BlockId>(b);
            edge(in(id), out(id));
            if (block.handler != il::kNoBlock) edge(in(id), in(block.handler));
            if (block.instructions.empty()) continue;
            const il::Instruction& term = block.instructions.back();
            if (term.op == il::Op::Jump || term.op == il::Op::Branch) edge(out(id), in(term.target.block));
            if (term.op == il::Op::Branch) edge(out(id), in(term.elseTarget.block));
        }
        // Reverse postorder, iteratively (a three.js function can be hundreds
        // of blocks deep).
        std::vector<uint32_t> postorder;
        std::vector<uint8_t> seen(n, 0);
        std::vector<std::pair<uint32_t, size_t>> stack;
        if (n == 0) return;
        seen[0] = 1;
        stack.push_back({0, 0});
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < succs[node].size()) {
                const uint32_t s = succs[node][next++];
                if (!seen[s]) {
                    seen[s] = 1;
                    stack.push_back({s, 0});
                }
                continue;
            }
            postorder.push_back(node);
            stack.pop_back();
        }
        const std::vector<uint32_t> rpo(postorder.rbegin(), postorder.rend());
        rpoIndex.assign(n, UINT32_MAX);
        for (uint32_t i = 0; i < rpo.size(); ++i) rpoIndex[rpo[i]] = i;
        constexpr uint32_t kNone = UINT32_MAX;
        idom.assign(n, kNone);
        idom[0] = 0;
        auto intersect = [&](uint32_t a, uint32_t b) {
            while (a != b) {
                while (rpoIndex[a] > rpoIndex[b]) a = idom[a];
                while (rpoIndex[b] > rpoIndex[a]) b = idom[b];
            }
            return a;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (uint32_t v : rpo) {
                if (v == 0) continue;
                uint32_t nd = kNone;
                for (uint32_t p : preds[v]) {
                    if (rpoIndex[p] == UINT32_MAX || idom[p] == kNone) continue;
                    nd = nd == kNone ? p : intersect(nd, p);
                }
                if (nd != kNone && idom[v] != nd) {
                    idom[v] = nd;
                    changed = true;
                }
            }
        }
    }

    bool reachable(uint32_t v) const { return v < rpoIndex.size() && rpoIndex[v] != UINT32_MAX; }

    bool dominates(uint32_t a, uint32_t b) const {
        if (!reachable(a) || !reachable(b)) return false;
        for (uint32_t cur = b;; cur = idom[cur]) {
            if (cur == a) return true;
            if (cur == 0) return false;
        }
    }
};

// A site's claim: one identity cell, or one family range.
struct Claim {
    uint32_t slot = 0;
    uint32_t cell = 0;
    uint32_t familyLo = il::Instruction::kNoFamily;
    uint32_t familySpan = 0;
    uint32_t icIndex = 0;
    bool regrow = false;

    bool same(const Claim& o) const {
        if (familyLo != il::Instruction::kNoFamily || o.familyLo != il::Instruction::kNoFamily) {
            return familyLo == o.familyLo && familySpan == o.familySpan;
        }
        return cell == o.cell;
    }
};

Claim claimOf(const il::Instruction& inst) {
    Claim c;
    c.slot = inst.staticSlot;
    c.icIndex = inst.icIndex;
    if (inst.familyLo != il::Instruction::kNoFamily) {
        c.familyLo = inst.familyLo;
        c.familySpan = inst.familySpan;
    } else {
        c.cell = inst.staticCellIndex;
        c.regrow = inst.staticRegrow;
    }
    return c;
}

struct Site {
    il::BlockId block = 0;
    uint32_t index = 0;
    il::ValueId recv = il::kNoValue;
    Claim claim;
    uint32_t stretch = 0;  // the memo stretch it stands in (memoStretches)
};

// A static site the backend may cover: a read, or an assignment of a Number
// box (every slot representation takes one with no barrier), with a claim of
// its own and nothing else answering it first.
bool candidate(const il::Instruction& inst, const std::vector<il::Type>& types,
               const std::vector<const il::Instruction*>& defOf) {
    if (inst.op != il::Op::PropGet && inst.op != il::Op::PropSet) return false;
    if (inst.staticSlot == il::Instruction::kNoStaticSlot || inst.shapeGuarded) return false;
    if (inst.directTarget != il::Instruction::kNoDirectTarget || inst.operands.empty()) return false;
    const il::ValueId recv = inst.operands[0];
    if (recv >= types.size() || types[recv] != il::Type::Dynamic) return false;
    if (inst.op == il::Op::PropGet) return inst.operands.size() == 1 && inst.result != il::kNoValue;
    if (il::isDefineWrite(inst) || inst.operands.size() != 2) return false;
    if (inst.transitionRunCell != il::Instruction::kNoTransitionRun) return false;
    const il::ValueId val = inst.operands[1];
    const il::Instruction* def = val < defOf.size() ? defOf[val] : nullptr;
    return def != nullptr && def->op == il::Op::Box && def->boxType == il::Type::F64;
}

// Whether the backend's shape memo survives `inst` (il_shape_memo.cpp): a
// static site carries it across its own join, and an instruction that can
// neither run code nor allocate leaves it alone. The memo's own list is
// narrower; reading it wider here only leaves a member local (uncovered)
// that the memo would not have helped, which is today's code.
bool keepsMemo(const il::Instruction& inst) {
    if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
        inst.staticSlot != il::Instruction::kNoStaticSlot && inst.directTarget == il::Instruction::kNoDirectTarget) {
        return true;
    }
    return !il::canThrow(inst) && !il::canCollect(inst);
}

// The backend's SHAPE MEMO STRETCHES (il_shape_memo.cpp), numbered: a new
// one at the entry, at a block with other than one incoming edge or one a
// handler names, and after every instruction that forgets the memo; a block
// with one incoming edge continues its predecessor's. A static site that is
// not the first on its receiver in its stretch is answered by the memo
// already (a compare), so only a stretch's first visit to a receiver pays a
// guard that a fact could replace. By block, the stretch in front of each
// instruction.
std::vector<std::vector<uint32_t>> memoStretches(const il::Function& fn) {
    const size_t n = fn.blocks.size();
    std::vector<uint32_t> predCount(n, 0), pred(n, il::kNoBlock);
    std::vector<uint8_t> handler(n, 0);
    for (size_t b = 0; b < n; ++b) {
        const il::Block& block = fn.blocks[b];
        if (block.handler != il::kNoBlock && block.handler < n) handler[block.handler] = 1;
        if (block.instructions.empty()) continue;
        const il::Instruction& term = block.instructions.back();
        auto edge = [&](il::BlockId to) {
            if (to >= n) return;
            ++predCount[to];
            pred[to] = static_cast<il::BlockId>(b);
        };
        if (term.op == il::Op::Jump || term.op == il::Op::Branch) edge(term.target.block);
        if (term.op == il::Op::Branch) edge(term.elseTarget.block);
    }
    const Cfg cfg = buildCfg(fn);
    std::vector<std::vector<uint32_t>> at(n);
    std::vector<uint32_t> atEnd(n, UINT32_MAX);
    uint32_t next = 0;
    for (il::BlockId b : cfg.rpo) {
        const il::Block& block = fn.blocks[b];
        uint32_t cur = next++;
        if (predCount[b] == 1 && !handler[b] && pred[b] < b && atEnd[pred[b]] != UINT32_MAX) cur = atEnd[pred[b]];
        at[b].resize(block.instructions.size());
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            at[b][i] = cur;
            if (!keepsMemo(block.instructions[i])) cur = next++;
        }
        atEnd[b] = cur;
    }
    return at;
}

// Which claims get a fact at one anchor, and which remote members hold one.
// A claim is worth a fact when it answers the first visit of two or more
// stretches (one fact for several guards); a stretch is covered only whole,
// since a site after a covered one in the same stretch would find no keyed
// shape in the memo and pay a full guard. A stretch is covered only when one
// claim answers all of it: its guard asks the receiver's shape once and its
// other sites compare the memo's keyed shape, while every distinct fact is a
// compare of its own against a value kept across the calls (identity claims,
// one per key, made object_graph's `traverseDFS` pay three facts for one
// guard). And the claim is a FAMILY range: an identity guard is one load and
// one compare against the site's cell, about what the fact's own check costs
// (the epoch word, the fact reloaded across the calls, its conversion), so it
// saves nothing per site and the fact's making is a loss; a family guard reads
// the stamp and the family base and compares a range, and a fact pays where a
// claim is asked twice. Settled by iterating, as dropping a stretch can drop
// a claim below two.
std::vector<const Site*> selectCovered(const std::vector<const Site*>& remote, std::vector<Claim>& claims) {
    std::vector<uint32_t> stretchIds;
    std::vector<Claim> stretchClaim;  // by stretch: the first site's claim
    std::vector<uint8_t> stretchOn;
    for (const Site* t : remote) {
        const size_t st = static_cast<size_t>(std::find(stretchIds.begin(), stretchIds.end(), t->stretch) -
                                              stretchIds.begin());
        if (st == stretchIds.size()) {
            stretchIds.push_back(t->stretch);
            stretchClaim.push_back(t->claim);
            stretchOn.push_back(t->claim.familyLo != il::Instruction::kNoFamily ? 1 : 0);
        } else if (!stretchClaim[st].same(t->claim)) {
            stretchOn[st] = 0;
        }
    }
    auto stretchIndex = [&](uint32_t id) {
        return static_cast<size_t>(std::find(stretchIds.begin(), stretchIds.end(), id) - stretchIds.begin());
    };
    for (bool changed = true; changed;) {
        changed = false;
        claims.clear();
        std::vector<std::vector<size_t>> seenIn;  // by claim: stretches it occurs in
        for (const Site* t : remote) {
            const size_t st = stretchIndex(t->stretch);
            if (!stretchOn[st]) continue;
            size_t c = 0;
            while (c < claims.size() && !claims[c].same(t->claim)) ++c;
            if (c == claims.size()) {
                claims.push_back(t->claim);
                seenIn.emplace_back();
            }
            if (std::find(seenIn[c].begin(), seenIn[c].end(), st) == seenIn[c].end()) seenIn[c].push_back(st);
        }
        for (const Site* t : remote) {
            const size_t st = stretchIndex(t->stretch);
            if (!stretchOn[st]) continue;
            size_t c = 0;
            while (!claims[c].same(t->claim)) ++c;
            if (seenIn[c].size() < 2) {
                stretchOn[st] = 0;
                changed = true;
            }
        }
    }
    std::vector<Claim> kept;
    std::vector<const Site*> covered;
    for (const Site* t : remote) {
        if (!stretchOn[stretchIndex(t->stretch)]) continue;
        covered.push_back(t);
        bool have = false;
        for (const Claim& c : kept) have = have || c.same(t->claim);
        if (!have) kept.push_back(t->claim);
    }
    claims = std::move(kept);
    return covered;
}

bool matchesFilter(const std::string& name, const std::string& filter) {
    size_t start = 0;
    while (start <= filter.size()) {
        const size_t end = std::min(filter.find(',', start), filter.size());
        if (end > start && name.find(filter.substr(start, end - start)) != std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

struct FunctionCounts {
    uint32_t sites = 0;
    uint32_t anchors = 0;
    uint32_t facts = 0;
    uint32_t covered = 0;
    uint32_t local = 0;
};

// Rewrites one function in place. False when nothing changed.
bool rewriteFunction(il::Function& fn, FunctionCounts& counts) {
    std::vector<il::Type> types(fn.valueCount, il::Type::Void);
    std::vector<const il::Instruction*> defOf(fn.valueCount, nullptr);
    for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) types[p] = fn.params[p].type;
    for (const il::Block& block : fn.blocks) {
        for (const il::BlockParam& param : block.params) {
            if (param.id < types.size()) types[param.id] = param.type;
        }
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result == il::kNoValue || inst.result >= types.size()) continue;
            types[inst.result] = inst.type;
            defOf[inst.result] = &inst;
        }
    }

    // The candidate sites, by receiver. None inside a TRANSITION RUN
    // (transition_runs.h): a run is a fixed count of instructions the backend
    // lowers as one, and neither a fact in front of a member nor a member
    // holding one is anything it lowers.
    std::map<il::ValueId, std::vector<Site>> byRecv;
    const std::vector<std::vector<uint32_t>> stretchAt = memoStretches(fn);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& insts = fn.blocks[b].instructions;
        std::vector<uint8_t> inRun(insts.size(), 0);
        for (uint32_t i = 0; i < insts.size(); ++i) {
            if (insts[i].transitionRunCell == il::Instruction::kNoTransitionRun) continue;
            for (uint32_t k = i; k < insts.size() && k < i + insts[i].transitionRunLength; ++k) inRun[k] = 1;
        }
        for (uint32_t i = 0; i < insts.size(); ++i) {
            if (inRun[i] || !candidate(insts[i], types, defOf)) continue;
            const uint32_t stretch = i < stretchAt[b].size() ? stretchAt[b][i] : UINT32_MAX;
            byRecv[insts[i].operands[0]].push_back(
                Site{static_cast<il::BlockId>(b), i, insts[i].operands[0], claimOf(insts[i]), stretch});
            ++counts.sites;
        }
    }
    bool any = false;
    for (const auto& [recv, sites] : byRecv) {
        if (sites.size() >= 2) any = true;
    }
    if (!any) return false;

    const SplitDom dom(fn);
    auto siteDominates = [&](const Site& a, const Site& t) {
        if (a.block == t.block) return a.index < t.index;
        // Index order too: the backend emits blocks in index order and a
        // definition's block must come first (guard_region_cfg.h).
        return a.block < t.block && dom.dominates(SplitDom::out(a.block), SplitDom::in(t.block));
    };
    auto local = [&](const Site& a, const Site& t) { return a.stretch == t.stretch; };

    // Facts to insert, by block: (in front of instruction index, the fact).
    std::vector<std::vector<std::pair<uint32_t, il::Instruction>>> inserts(fn.blocks.size());
    il::ValueId next = fn.valueCount;
    for (auto& [recv, sitesRef] : byRecv) {
        if (sitesRef.size() < 2) continue;
        std::vector<Site> sites = sitesRef;
        // Reachable sites in dominance-compatible order: a dominator's start
        // comes before its dominatees' in reverse postorder.
        sites.erase(std::remove_if(sites.begin(), sites.end(),
                                   [&](const Site& s) { return !dom.reachable(SplitDom::in(s.block)); }),
                    sites.end());
        std::sort(sites.begin(), sites.end(), [&](const Site& x, const Site& y) {
            const uint32_t rx = dom.rpoIndex[SplitDom::in(x.block)];
            const uint32_t ry = dom.rpoIndex[SplitDom::in(y.block)];
            return rx != ry ? rx < ry : x.index < y.index;
        });
        std::vector<Site> anchors;
        std::vector<std::vector<Site>> members;
        for (const Site& s : sites) {
            size_t owner = SIZE_MAX;
            for (size_t k = 0; k < anchors.size() && owner == SIZE_MAX; ++k) {
                if (siteDominates(anchors[k], s)) owner = k;
            }
            if (owner == SIZE_MAX) {
                anchors.push_back(s);
                members.emplace_back();
            } else {
                members[owner].push_back(s);
            }
        }
        for (size_t k = 0; k < anchors.size(); ++k) {
            const Site& a = anchors[k];
            std::vector<Claim> claims;
            std::vector<il::ValueId> factOf;
            std::vector<const Site*> remote;
            for (const Site& t : members[k]) {
                if (local(a, t)) {
                    ++counts.local;
                    continue;
                }
                remote.push_back(&t);
            }
            remote = selectCovered(remote, claims);
            if (remote.empty()) continue;
            ++counts.anchors;
            for (const Claim& c : claims) {
                il::Instruction fact;
                fact.op = il::Op::LayoutFact;
                fact.type = il::Type::F64;
                fact.result = next++;
                fact.operands = {recv};
                fact.staticSlot = c.slot;
                fact.staticCellIndex = c.cell;
                fact.staticRegrow = c.regrow;
                fact.familyLo = c.familyLo;
                fact.familySpan = c.familySpan;
                fact.icIndex = c.icIndex;
                fact.span = fn.blocks[a.block].instructions[a.index].span;
                fact.inlineFrame = fn.blocks[a.block].instructions[a.index].inlineFrame;
                factOf.push_back(fact.result);
                inserts[a.block].emplace_back(a.index, std::move(fact));
                ++counts.facts;
            }
            for (const Site* t : remote) {
                size_t c = 0;
                while (!claims[c].same(t->claim)) ++c;
                il::Instruction& inst = fn.blocks[t->block].instructions[t->index];
                inst.operands.push_back(factOf[c]);
                inst.shapeGuarded = true;
                ++counts.covered;
            }
        }
    }
    if (counts.facts == 0) return false;

    // Insert back to front, so earlier indices stay put. Facts in front of
    // one anchor keep their order.
    for (size_t b = 0; b < inserts.size(); ++b) {
        auto& list = inserts[b];
        std::stable_sort(list.begin(), list.end(),
                         [](const auto& x, const auto& y) { return x.first > y.first; });
        auto& insts = fn.blocks[b].instructions;
        for (size_t i = 0; i < list.size();) {
            size_t j = i;
            while (j < list.size() && list[j].first == list[i].first) ++j;
            std::vector<il::Instruction> group;
            for (size_t k = i; k < j; ++k) group.push_back(std::move(list[k].second));
            insts.insert(insts.begin() + static_cast<ptrdiff_t>(list[i].first), group.begin(), group.end());
            i = j;
        }
    }
    fn.valueCount = next;
    return true;
}

}  // namespace

bool applyLayoutFacts(il::Module& module, LayoutFactStats* stats, const std::string& filter,
                      std::string* perFunction) {
    LayoutFactStats local;
    LayoutFactStats& s = stats ? *stats : local;
    bool changed = false;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) continue;
        FunctionCounts counts;
        // Only where something could change: most functions have no receiver
        // read twice under a claim. No whole-function dominance check after
        // (guard_region_cfg.h `dominanceHolds`): lowering's own output does
        // not meet its block-order rule everywhere, and the only uses this
        // adds — each fact read by sites its anchor dominates, in a block at
        // or after the anchor's — meet it by construction (`siteDominates`).
        uint32_t n = 0;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.staticSlot != il::Instruction::kNoStaticSlot && !inst.shapeGuarded) ++n;
            }
        }
        if (n < 2) continue;
        const bool did = rewriteFunction(fn, counts);
        if (counts.sites > 0) ++s.functions;
        s.sites += counts.sites;
        if (did) {
            changed = true;
            s.anchors += counts.anchors;
            s.facts += counts.facts;
            s.covered += counts.covered;
            s.local += counts.local;
        }
        if (perFunction && !filter.empty() && counts.sites > 0 && matchesFilter(fn.name, filter)) {
            *perFunction += "wpo-layout-fn " + fn.name + " sites " + std::to_string(counts.sites) + " covered " +
                            std::to_string(did ? counts.covered : 0) + " facts " +
                            std::to_string(did ? counts.facts : 0) + " local " + std::to_string(counts.local) + "\n";
        }
    }
    return changed;
}

std::string layoutFactReport(const LayoutFactStats& s) {
    return "wpo-layout: static sites " + std::to_string(s.sites) + " in " + std::to_string(s.functions) +
           " functions; covered by a layout fact " + std::to_string(s.covered) + " (facts " +
           std::to_string(s.facts) + " at " + std::to_string(s.anchors) + " anchors), left local " +
           std::to_string(s.local) + "\n";
}

}  // namespace bronze::lower
