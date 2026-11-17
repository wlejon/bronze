#include "opt/core/fact_env.h"

#include <algorithm>
#include <iterator>

namespace bronze::opt {

void FactEnv::push() { scopes_.push_back(log_.size()); }

void FactEnv::pop() {
    if (scopes_.empty()) return;
    const size_t mark = scopes_.back();
    scopes_.pop_back();
    // Undo newest first. Additions are LIFO across scopes, so an added fact
    // is always the last one stored when its entry comes off the log.
    while (log_.size() > mark) {
        const LogEntry entry = log_.back();
        log_.pop_back();
        if (!entry.added) {
            alive_[entry.fact] = 1;
            continue;
        }
        unindexFact(entry.fact);
        if (!dependent_.empty() && dependent_.back() == entry.fact) dependent_.pop_back();
        facts_.pop_back();
        alive_.pop_back();
    }
}

void FactEnv::indexFact(uint32_t i) {
    const Fact& f = facts_[i];
    byValue_[f.value].push_back(i);
    if (f.other != kNoValue && f.other != f.value) byValue_[f.other].push_back(i);
}

void FactEnv::unindexFact(uint32_t i) {
    const Fact& f = facts_[i];
    auto drop = [&](ValueId v) {
        auto it = byValue_.find(v);
        if (it == byValue_.end() || it->second.empty()) return;
        if (it->second.back() == i) it->second.pop_back();
        if (it->second.empty()) byValue_.erase(it);
    };
    drop(f.value);
    if (f.other != kNoValue && f.other != f.value) drop(f.other);
}

void FactEnv::add(const Fact& fact) {
    if (fact.value == kNoValue) return;
    const uint32_t i = static_cast<uint32_t>(facts_.size());
    facts_.push_back(fact);
    alive_.push_back(1);
    if (!fact.dependsOn.empty()) dependent_.push_back(i);
    indexFact(i);
    log_.push_back(LogEntry{i, true});
}

void FactEnv::addAll(const std::vector<Fact>& facts) {
    for (const Fact& f : facts) add(f);
}

void FactEnv::killLocs(il::LocSet writes) {
    if (writes.empty()) return;
    for (uint32_t i : dependent_) {
        if (alive_[i] && facts_[i].dependsOn.intersects(writes)) {
            alive_[i] = 0;
            log_.push_back(LogEntry{i, false});
        }
    }
}

void FactEnv::killBy(const il::Effects& effects) {
    killLocs(effects.callsUnknown ? il::LocSet::all() : effects.writes);
}

void FactEnv::stepOver(const il::Instruction& inst) { killBy(il::effectsOf(inst)); }

std::optional<std::vector<uint32_t>> FactEnv::shapeBound(ValueId v, std::vector<uint32_t>* excluded) const {
    const auto it = byValue_.find(v);
    if (it == byValue_.end()) return std::nullopt;
    std::optional<std::vector<uint32_t>> bound;
    auto meet = [&](std::vector<uint32_t> cells) {
        if (!bound) {
            bound = std::move(cells);
            return;
        }
        std::vector<uint32_t> both;
        std::set_intersection(bound->begin(), bound->end(), cells.begin(), cells.end(), std::back_inserter(both));
        *bound = std::move(both);
    };
    for (const uint32_t i : it->second) {
        const Fact& f = facts_[i];
        if (!alive_[i] || f.value != v) continue;
        if (f.kind == FactKind::Shape) {
            if (f.holds) {
                meet({static_cast<uint32_t>(f.imm)});
            } else if (excluded != nullptr) {
                excluded->push_back(static_cast<uint32_t>(f.imm));
            }
        } else if (f.kind == FactKind::ShapeSet && f.holds) {
            meet(f.cells);
        }
    }
    return bound;
}

std::optional<std::vector<uint32_t>> FactEnv::shapeSet(ValueId v) const {
    std::vector<uint32_t> excluded;
    std::optional<std::vector<uint32_t>> bound = shapeBound(v, &excluded);
    if (!bound) return std::nullopt;
    std::erase_if(*bound, [&](uint32_t c) { return std::find(excluded.begin(), excluded.end(), c) != excluded.end(); });
    return bound;
}

std::optional<bool> FactEnv::known(FactKind kind, ValueId v, int64_t imm, ValueId other) const {
    // One object has one shape: the bound of the Shape and ShapeSet facts
    // answers, and a failed test of the cell itself.
    if (kind == FactKind::Shape) {
        std::vector<uint32_t> excluded;
        std::optional<std::vector<uint32_t>> bound = shapeBound(v, &excluded);
        const auto cell = static_cast<uint32_t>(imm);
        if (std::find(excluded.begin(), excluded.end(), cell) != excluded.end()) return false;
        if (!bound) return std::nullopt;
        std::erase_if(*bound,
                      [&](uint32_t c) { return std::find(excluded.begin(), excluded.end(), c) != excluded.end(); });
        if (std::find(bound->begin(), bound->end(), cell) == bound->end()) return false;
        if (bound->size() == 1) return true;
        return std::nullopt;
    }
    const auto it = byValue_.find(v);
    if (it == byValue_.end()) return std::nullopt;
    const auto& list = it->second;
    for (auto r = list.rbegin(); r != list.rend(); ++r) {
        const uint32_t i = *r;
        if (!alive_[i]) continue;
        const Fact& f = facts_[i];
        if (f.kind != kind) continue;
        switch (kind) {
            case FactKind::InBounds:
                if (f.value == v && f.other == other) return f.holds;
                continue;
            case FactKind::PtrEq:
                if ((f.value == v && f.other == other) || (f.value == other && f.other == v)) {
                    return f.holds;
                }
                continue;
            default:
                break;
        }
        if (f.value != v) continue;
        switch (kind) {
            case FactKind::BoolValue:
                return f.imm == imm;
            // One function has one code: knowing it is X answers "is it Y"
            // with no.
            case FactKind::Callee:
                if (f.imm == imm) return f.holds;
                if (f.holds) return false;
                continue;
            // `is.dense_array v, k` covers every index up to k; failing it at
            // k fails it at every larger k. The NUMBERS form (il_op.h
            // kDenseNumbers) answers the plain one, never the reverse.
            case FactKind::DenseArray: {
                const int64_t fi = f.imm & il::kDenseIndexMask, qi = imm & il::kDenseIndexMask;
                const bool fn = (f.imm & il::kDenseNumbers) != 0, qn = (imm & il::kDenseNumbers) != 0;
                if (f.holds && fi >= qi && (fn || !qn)) return true;
                if (!f.holds && fi <= qi && (qn || !fn)) return false;
                continue;
            }
            case FactKind::TypedArray:
                if (f.imm == imm) return f.holds;
                continue;
            default:
                return f.holds;
        }
    }
    return std::nullopt;
}

bool FactEnv::inRange(ValueId base, ValueId length, int32_t lo, int32_t hi) const {
    const auto it = byValue_.find(base);
    if (it == byValue_.end()) return false;
    for (const uint32_t i : it->second) {
        const Fact& f = facts_[i];
        if (!alive_[i] || f.kind != FactKind::InRange || !f.holds || f.value != base) continue;
        if (length != kNoValue && f.other != length) continue;
        if (rangeLo(f.imm) <= lo && hi <= rangeHi(f.imm)) return true;
    }
    return false;
}

bool FactEnv::sumFits(ValueId base, int32_t k) const {
    const auto it = byValue_.find(base);
    if (it == byValue_.end()) return false;
    for (const uint32_t i : it->second) {
        const Fact& f = facts_[i];
        if (!alive_[i] || f.kind != FactKind::InRange || !f.holds || f.value != base || f.other == kNoValue) continue;
        if (rangeLo(f.imm) <= k && static_cast<int64_t>(k) <= static_cast<int64_t>(rangeHi(f.imm)) + 1) return true;
    }
    return false;
}

std::optional<bool> FactEnv::boolValue(ValueId v) const {
    if (known(FactKind::BoolValue, v, 1) == true) return true;
    if (known(FactKind::BoolValue, v, 0) == true) return false;
    return std::nullopt;
}

namespace {

// The holding fact of `kind` about `v`, newest first, if any.
const Fact* newestHolding(const std::vector<Fact>& facts, const std::vector<uint8_t>& alive,
                          const std::vector<uint32_t>& list, ValueId v, FactKind kind) {
    for (auto r = list.rbegin(); r != list.rend(); ++r) {
        const Fact& f = facts[*r];
        if (alive[*r] && f.kind == kind && f.value == v && f.holds) return &f;
    }
    return nullptr;
}

}  // namespace

bool FactEnv::isObject(ValueId v) const {
    const auto it = byValue_.find(v);
    if (it == byValue_.end()) return false;
    for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
        const Fact& f = facts_[*r];
        if (!alive_[*r] || f.value != v || !f.holds) continue;
        switch (f.kind) {
            case FactKind::Object:
            case FactKind::Shape:
            case FactKind::ShapeSet:
            case FactKind::DenseArray:
            case FactKind::PlainArray:
            case FactKind::TypedArray:
            case FactKind::Array:
            case FactKind::Callee:
                return true;
            // A primitive tag rules it out.
            case FactKind::I32:
            case FactKind::Number:
            case FactKind::Nullish:
            case FactKind::Hole:
                return false;
            default:
                break;
        }
    }
    return false;
}

ValueTag FactEnv::tagOf(ValueId v) const {
    if (isI32(v)) return ValueTag::I32;
    if (isNumber(v)) return ValueTag::Number;
    if (isObject(v)) return ValueTag::Object;
    if (known(FactKind::Nullish, v) == true) return ValueTag::Nullish;
    return ValueTag::Unknown;
}

std::optional<uint32_t> FactEnv::shapeOf(ValueId v) const {
    const std::optional<std::vector<uint32_t>> set = shapeSet(v);
    if (!set || set->size() != 1) return std::nullopt;
    return set->front();
}

std::optional<uint32_t> FactEnv::calleeOf(ValueId v) const {
    const auto it = byValue_.find(v);
    if (it == byValue_.end()) return std::nullopt;
    const Fact* f = newestHolding(facts_, alive_, it->second, v, FactKind::Callee);
    if (f == nullptr) return std::nullopt;
    return static_cast<uint32_t>(f->imm);
}

bool FactEnv::isDenseArray(ValueId v, int32_t index) const {
    return known(FactKind::DenseArray, v, index) == true;
}

bool FactEnv::isArray(ValueId v) const {
    if (known(FactKind::Array, v) == true || known(FactKind::PlainArray, v) == true) return true;
    const auto it = byValue_.find(v);
    if (it == byValue_.end()) return false;
    return newestHolding(facts_, alive_, it->second, v, FactKind::DenseArray) != nullptr;
}

bool FactEnv::isTypedArray(ValueId v) const {
    const auto it = byValue_.find(v);
    if (it == byValue_.end()) return false;
    return newestHolding(facts_, alive_, it->second, v, FactKind::TypedArray) != nullptr;
}

std::vector<Fact> FactEnv::live() const {
    std::vector<Fact> out;
    for (size_t i = 0; i < facts_.size(); ++i) {
        if (alive_[i]) out.push_back(facts_[i]);
    }
    return out;
}

std::vector<il::LocSet> blockWrites(const il::Function& fn) {
    std::vector<il::LocSet> out(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            const il::Effects e = il::effectsOf(inst);
            out[b] |= e.callsUnknown ? il::LocSet::all() : e.writes;
        }
    }
    return out;
}

il::LocSet writesBetween(const Cfg& cfg, const std::vector<il::LocSet>& writes, BlockId from,
                         BlockId to) {
    // Backwards from `to`'s predecessors, stopping at `from`. Every block met
    // is dominated by `from` (a path around it would reach `to` around it
    // too), so the walk stays inside the region between the two.
    il::LocSet out;
    std::vector<uint8_t> seen(cfg.size(), 0);
    std::vector<BlockId> work(cfg.preds(to).begin(), cfg.preds(to).end());
    while (!work.empty()) {
        const BlockId b = work.back();
        work.pop_back();
        if (b == from || seen[b] || !cfg.reachable(b)) continue;
        seen[b] = 1;
        out |= writes[b];
        if (out.isAll()) return out;
        for (BlockId p : cfg.preds(b)) work.push_back(p);
    }
    return out;
}

void walkWithFacts(const il::Function& fn, const Cfg& cfg, const DomTree& dom, const DefUse& defs,
                   FactEnv& env, const std::function<void(BlockId, FactEnv&)>& visit) {
    if (fn.blocks.empty()) return;
    const std::vector<il::LocSet> writes = blockWrites(fn);
    struct Frame {
        BlockId block;
        bool leaving;
    };
    std::vector<Frame> stack{{0, false}};
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.leaving) {
            env.pop();
            continue;
        }
        const BlockId b = frame.block;
        env.push();
        stack.push_back({b, true});
        if (b != 0) {
            // One way in: that edge's facts, and nothing lies between the
            // dominator's end and here. A join: whatever any path from the
            // dominator writes kills what depended on it.
            if (cfg.inEdges(b).size() == 1) {
                env.addAll(entryFacts(fn, cfg, defs, b));
            } else {
                env.killLocs(writesBetween(cfg, writes, dom.idom(b), b));
            }
        }
        visit(b, env);
        const auto& kids = dom.children(b);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back({*it, false});
    }
}

}  // namespace bronze::opt
