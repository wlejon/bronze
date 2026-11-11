#include "opt/wpo/shape.h"

#include "abi/bronze_abi.h"

#include <algorithm>

namespace bronze::wpo {

namespace {

// How deep a chain of constructors and the calls they make the walk follows.
constexpr int kMaxDepth = 8;
// The runtime's dictionary threshold is 1024 slots; well below it.
constexpr size_t kMaxSeq = 900;

bool isPureUse(il::Op op) {
    switch (op) {
        case il::Op::StrictEq: case il::Op::TypeOf: case il::Op::IsNullish: case il::Op::IsNumber:
        case il::Op::IsDenseArray: case il::Op::IsTypedArray: case il::Op::IsNumberArray: case il::Op::IsArray:
        case il::Op::IsPlainArray:
        case il::Op::MethodIcIs: case il::Op::AccessorIcIs: case il::Op::IsShape: case il::Op::KeepAlive:
        case il::Op::Print:
        case il::Op::PrintErr: case il::Op::MethodSlotFn: case il::Op::ClosureIs: case il::Op::Branch:
        case il::Op::PatternCheck: case il::Op::ElemGetTyped: case il::Op::ElemSetTyped:
        case il::Op::CensusRecord: case il::Op::FeedbackRecord: case il::Op::PinGuard:
            return true;
        default:
            return false;
    }
}

}  // namespace

int ObjShape::slotOf(uint32_t key) const {
    uint32_t slot = 0;
    for (const auto& e : seq) {
        const uint32_t width = e.accessor ? 2u : 1u;
        if (slot < BRONZE_ABI_OBJ_OVERFLOW_SLOT && slot + width > inlineSlots) slot = BRONZE_ABI_OBJ_OVERFLOW_SLOT;
        if (e.key == key) return static_cast<int>(slot);
        slot += width;
    }
    return -1;
}

namespace {

uint32_t clampInline(uint32_t n) {
    if (n < BRONZE_ABI_OBJ_INLINE_SLOTS) return BRONZE_ABI_OBJ_INLINE_SLOTS;
    return n > BRONZE_ABI_OBJ_OVERFLOW_SLOT ? BRONZE_ABI_OBJ_OVERFLOW_SLOT : n;
}

}  // namespace

ShapeAnalysis::ShapeAnalysis(Solver& solver) : solver_(solver) {
    protoKey_ = solver_.key("prototype");
}

void ShapeAnalysis::run() {
    shapes_.assign(solver_.objectCount(), ObjShape{});
    for (ObjId id = 0; id < solver_.objectCount(); ++id) computeObject(id);
}

bool ShapeAnalysis::singleShape(ObjId id) const {
    const ObjShape& s = shapes_[id];
    const AObj& o = solver_.object(id);
    const bool escaped = o.escaped && !solver_.options().predict;
    return s.modelled && s.exact && !s.partial && s.extras.empty() && !escaped && !o.anyKey &&
           !o.deleted && !o.attrsChanged && !o.protoSet;
}

bool ShapeAnalysis::slotsHold(ObjId id) const {
    const ObjShape& s = shapes_[id];
    const AObj& o = solver_.object(id);
    return s.modelled && !o.deleted && !o.protoSet && s.seq.size() < kMaxSeq;
}

void ShapeAnalysis::inexact(State& s, const std::string& why) {
    if (s.exact && s.why.empty()) s.why = why;
    s.exact = false;
}

bool ShapeAnalysis::join(State& into, const State& from) {
    if (!from.valid) return false;
    if (!into.valid) {
        into = from;
        return true;
    }
    bool changed = false;
    size_t common = 0;
    while (common < into.seq.size() && common < from.seq.size() && into.seq[common] == from.seq[common]) {
        ++common;
    }
    const bool same = common == into.seq.size() && common == from.seq.size();
    if (common < into.seq.size()) {
        into.seq.resize(common);
        changed = true;
    }
    if (into.exact && (!from.exact || !same)) {
        inexact(into, from.exact ? std::string("paths add different keys") : from.why);
        changed = true;
    }
    if (from.exposed && !into.exposed) {
        into.exposed = true;
        changed = true;
    }
    if (from.partial && !into.partial) {
        into.partial = true;
        changed = true;
    }
    return changed;
}

bool ShapeAnalysis::chainHasAccessor(uint32_t key) const {
    std::vector<ObjId> work;
    const AObj& t = solver_.object(target_);
    if (t.proto.topObj) return true;
    work.assign(t.proto.objs.begin(), t.proto.objs.end());
    std::vector<ObjId> seen;
    while (!work.empty()) {
        const ObjId o = work.back();
        work.pop_back();
        if (std::find(seen.begin(), seen.end(), o) != seen.end()) continue;
        seen.push_back(o);
        const AObj& a = solver_.object(o);
        if ((a.escaped && !solver_.options().predict) || a.proto.topObj || a.accessors.count(key) != 0) return true;
        if (a.kind == ObjKind::Builtin && solver_.keys().name(key) == "__proto__") return true;
        for (ObjId p : a.proto.objs) work.push_back(p);
    }
    return false;
}

void ShapeAnalysis::computeObject(ObjId id) {
    const AObj& o = solver_.object(id);
    if (o.kind != ObjKind::Instance && o.kind != ObjKind::Plain && o.kind != ObjKind::Proto) return;
    if (o.site == UINT32_MAX) return;
    const auto loc = solver_.insts().locate(o.site);
    const auto& mod = solver_.module();
    const auto& creator = mod.functions[loc.fn].blocks[loc.block].instructions[loc.inst];
    target_ = id;
    stack_.clear();
    State in;
    in.valid = true;
    WalkOut out;
    if (o.kind == ObjKind::Instance) {
        const il::Function& f = mod.functions[o.fn];
        std::vector<il::ValueId> seeds;
        if (f.needsThis) seeds.push_back(static_cast<il::ValueId>(f.needsEnv ? 1 : 0));
        out = walk(o.fn, 0, 0, seeds, {}, in, 0);
    } else if (o.kind == ObjKind::Plain) {
        if (creator.op != il::Op::CreateObject) return;
        out = walk(loc.fn, loc.block, loc.inst + 1, {creator.result}, {}, in, 0);
    } else {
        if (creator.op != il::Op::CreateFunction) return;
        in.seq.push_back(SeqEntry{solver_.key("constructor"), false});
        out = walk(loc.fn, loc.block, loc.inst + 1, {}, {creator.result}, in, 0);
    }
    ObjShape& s = shapes_[id];
    s.modelled = true;
    if (o.kind == ObjKind::Plain) {
        s.inlineSlots = clampInline(creator.immI32 > 0 ? static_cast<uint32_t>(creator.immI32) : 0u);
    } else if (o.kind == ObjKind::Instance && o.fn < mod.functions.size()) {
        const uint32_t flags = mod.functions[o.fn].fnFlags;
        s.inlineSlots =
            clampInline((flags >> BRONZE_ABI_FN_INLINE_SLOTS_SHIFT) & BRONZE_ABI_FN_INLINE_SLOTS_MASK);
    } else if (o.fn < mod.functions.size() && (mod.functions[o.fn].fnFlags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) != 0) {
        s.inlineSlots = BRONZE_ABI_OBJ_OVERFLOW_SLOT;
    }
    s.seq = out.exit.seq;
    s.exact = out.exit.valid && out.exit.exact;
    s.partial = out.exit.partial;
    s.why = out.exit.valid ? out.exit.why : std::string("construction never completes");
    auto inSeq = [&](uint32_t k) {
        return std::any_of(s.seq.begin(), s.seq.end(), [&](const SeqEntry& e) { return e.key == k; });
    };
    for (const auto& [k, v] : o.fields) {
        if (!solver_.keys().isIndex(k) && !inSeq(k)) s.extras.push_back(k);
    }
    for (const auto& [k, a] : o.accessors) {
        if (!inSeq(k) && std::find(s.extras.begin(), s.extras.end(), k) == s.extras.end()) {
            s.extras.push_back(k);
        }
    }
}

bool ShapeAnalysis::hasExoticProtoChain(ObjId id) const {
    const AObj& start = solver_.object(id);
    if (start.protoSet || start.proto.topObj) return true;
    std::vector<ObjId> work(start.proto.objs.begin(), start.proto.objs.end());
    std::vector<ObjId> seen;
    while (!work.empty()) {
        const ObjId o = work.back();
        work.pop_back();
        if (std::find(seen.begin(), seen.end(), o) != seen.end()) continue;
        seen.push_back(o);
        const AObj& a = solver_.object(o);
        if (a.protoSet || a.proto.topObj) return true;
        if (a.kind == ObjKind::Builtin) {
            if (solver_.catalog().def(a.builtin).path != "Object.prototype") return true;
            continue;
        }
        if (a.kind != ObjKind::Proto && a.kind != ObjKind::Plain && a.kind != ObjKind::Instance) {
            return true;
        }
        for (ObjId p : a.proto.objs) work.push_back(p);
    }
    return false;
}

void ShapeAnalysis::upgradeInstanceInlineSlots(il::Module& mod) {
    // Exotic subclasses (`extends Array`, `extends Map`, `extends Error`, ...)
    // keep the runtime's fixed 4-slot layout (`inline_slots.cpp`) because their
    // internal fields sit immediately after the fourth inline slot.
    std::vector<uint8_t> exoticCtor(mod.functions.size(), 0);
    std::vector<uint32_t> wantedCap(mod.functions.size(), 0);
    for (ObjId id = 0; id < shapes_.size(); ++id) {
        const ObjShape& s = shapes_[id];
        const AObj& o = solver_.object(id);
        if (!s.modelled || o.kind != ObjKind::Instance || o.fn >= mod.functions.size()) continue;
        const uint32_t flags = mod.functions[o.fn].fnFlags;
        if ((flags & BRONZE_ABI_FN_FLAG_CONSTRUCT) == 0) continue;
        if (hasExoticProtoChain(id)) {
            exoticCtor[o.fn] = 1;
            continue;
        }
        uint32_t width = 0;
        for (const SeqEntry& e : s.seq) width += e.accessor ? 2u : 1u;
        for (uint32_t k : s.extras) width += o.accessors.count(k) != 0 ? 2u : 1u;
        // Instances that escape or acquire properties after construction get
        // expando slack so caller-added fields stay inline instead of allocating
        // an overflow SlotBlock.
        if (o.escaped || o.anyKey || !s.exact || !s.extras.empty()) width += 4u;
        wantedCap[o.fn] = std::max(wantedCap[o.fn], clampInline(width));
    }

    constexpr uint32_t kInlineMask =
        uint32_t{BRONZE_ABI_FN_INLINE_SLOTS_MASK} << BRONZE_ABI_FN_INLINE_SLOTS_SHIFT;
    for (size_t fnIdx = 0; fnIdx < mod.functions.size(); ++fnIdx) {
        if (exoticCtor[fnIdx] || wantedCap[fnIdx] == 0) continue;
        uint32_t& flags = mod.functions[fnIdx].fnFlags;
        const uint32_t curCap =
            clampInline((flags >> BRONZE_ABI_FN_INLINE_SLOTS_SHIFT) & BRONZE_ABI_FN_INLINE_SLOTS_MASK);
        if (wantedCap[fnIdx] > curCap) {
            flags = (flags & ~kInlineMask) | (wantedCap[fnIdx] << BRONZE_ABI_FN_INLINE_SLOTS_SHIFT);
        } else {
            wantedCap[fnIdx] = curCap;
        }
    }

    for (ObjId id = 0; id < shapes_.size(); ++id) {
        ObjShape& s = shapes_[id];
        const AObj& o = solver_.object(id);
        if (!s.modelled || o.kind != ObjKind::Instance || o.fn >= mod.functions.size()) continue;
        if (exoticCtor[o.fn] || wantedCap[o.fn] == 0) continue;
        s.inlineSlots = wantedCap[o.fn];
    }
}

ShapeAnalysis::WalkOut ShapeAnalysis::walk(uint32_t fnIdx, uint32_t startBlock, uint32_t startInst,
                                           const std::vector<il::ValueId>& seeds,
                                           const std::vector<il::ValueId>& fnSeeds, const State& in,
                                           int depth) {
    WalkOut out;
    const auto& mod = solver_.module();
    const il::Function& fn = mod.functions[fnIdx];
    // An edge out of a block control never reaches feeds nothing.
    auto live = [&](const il::Block& block) {
        return solver_.blockLive(fnIdx, static_cast<uint32_t>(&block - fn.blocks.data()));
    };
    if (depth > kMaxDepth || std::find(stack_.begin(), stack_.end(), fnIdx) != stack_.end() ||
        fn.isExternal || fn.blocks.empty()) {
        out.exit = in;
        out.exit.exposed = true;
        inexact(out.exit, "a recursive or too deep constructor chain");
        return out;
    }
    stack_.push_back(fnIdx);

    // ---- which SSA values must be the object ----
    const size_t nv = fn.valueCount;
    std::vector<uint8_t> alias(nv, 0), fnAlias(nv, 0), use(nv, 0);
    for (il::ValueId s : seeds) if (s < nv) alias[s] = 1;
    for (il::ValueId s : fnSeeds) if (s < nv) fnAlias[s] = 1;
    std::vector<uint8_t> paramCand(nv, 0);
    for (const auto& block : fn.blocks) {
        for (const auto& p : block.params) if (p.id < nv) paramCand[p.id] = 1;
        for (const auto& inst : block.instructions) {
            for (il::ValueId v : inst.operands) if (v < nv) use[v] = 1;
            for (il::ValueId v : inst.target.args) if (v < nv) use[v] = 1;
            for (il::ValueId v : inst.elseTarget.args) if (v < nv) use[v] = 1;
        }
    }
    auto superReturnsNoObject = [&](const il::Instruction& inst) {
        const auto c = solver_.callees(solver_.value(fnIdx, inst.operands[0]));
        if (c.unknown || c.builtin || c.fns.empty()) return false;
        for (uint32_t g : c.fns) {
            if (solver_.returned(g).hasObjects()) return false;
        }
        return true;
    };
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.result == il::kNoValue || inst.result >= nv || alias[inst.result]) continue;
                const il::ValueId a0 = inst.operands.empty() ? il::kNoValue : inst.operands[0];
                bool derived = false;
                if (inst.op == il::Op::PropGet && a0 < nv && fnAlias[a0] && inst.keyIndex == protoKey_) derived = true;
                if (inst.op == il::Op::PatternCheck && a0 < nv && alias[a0]) derived = true;
                if (inst.op == il::Op::SuperCall && inst.operands.size() > 1 && inst.operands[1] < nv &&
                    alias[inst.operands[1]] && superReturnsNoObject(inst)) {
                    derived = true;
                }
                if (derived) {
                    alias[inst.result] = 1;
                    grew = true;
                }
            }
        }
    }
    // Block parameters, optimistically: a parameter is the object when every
    // edge passes the object or such a parameter. Pruned to a fixpoint.
    for (bool pruned = true; pruned;) {
        pruned = false;
        auto check = [&](const il::BlockTarget& t) {
            if (t.block >= fn.blocks.size()) return;
            const auto& params = fn.blocks[t.block].params;
            for (size_t i = 0; i < params.size(); ++i) {
                const il::ValueId p = params[i].id;
                if (p >= nv || !paramCand[p]) continue;
                const il::ValueId a = i < t.args.size() ? t.args[i] : il::kNoValue;
                if (a >= nv || !(alias[a] || paramCand[a])) {
                    paramCand[p] = 0;
                    pruned = true;
                }
            }
        };
        for (const auto& block : fn.blocks) {
            if (block.instructions.empty() || !live(block)) continue;
            const auto& last = block.instructions.back();
            if (last.op == il::Op::Jump) check(last.target);
            if (last.op == il::Op::Branch) {
                check(last.target);
                check(last.elseTarget);
            }
        }
    }
    // A parameter that survived but no edge reaches is not an alias.
    {
        std::vector<uint8_t> fed(nv, 0);
        for (const auto& block : fn.blocks) {
            if (block.instructions.empty() || !live(block)) continue;
            const auto& last = block.instructions.back();
            auto mark = [&](const il::BlockTarget& t) {
                if (t.block >= fn.blocks.size()) return;
                for (const auto& p : fn.blocks[t.block].params) if (p.id < nv) fed[p.id] = 1;
            };
            if (last.op == il::Op::Jump) mark(last.target);
            if (last.op == il::Op::Branch) {
                mark(last.target);
                mark(last.elseTarget);
            }
        }
        for (il::ValueId v = 0; v < nv; ++v) {
            if (paramCand[v] && fed[v]) alias[v] = 1;
        }
    }
    auto isAlias = [&](il::ValueId v) { return v < nv && (alias[v] || fnAlias[v]); };

    // ---- the dataflow over the window ----
    std::vector<State> blockIn(fn.blocks.size());
    std::vector<uint32_t> work;
    std::vector<uint8_t> queued(fn.blocks.size(), 0);
    blockIn[startBlock] = in;
    work.push_back(startBlock);
    queued[startBlock] = 1;
    bool firstStart = true;
    size_t aliasRets = 0;
    bool excExposed = false;
    auto flow = [&](uint32_t succ, const State& s) {
        if (succ >= fn.blocks.size() || !solver_.blockLive(fnIdx, succ)) return;
        if (succ == startBlock && startInst > 0) {
            // Back to the creating instruction: a new object from here on.
            join(out.exit, s);
            return;
        }
        if (join(blockIn[succ], s) && !queued[succ]) {
            queued[succ] = 1;
            work.push_back(succ);
        }
    };

    while (!work.empty()) {
        const uint32_t b = work.back();
        work.pop_back();
        queued[b] = 0;
        const auto& block = fn.blocks[b];
        State s = blockIn[b];
        if (!s.valid) continue;
        const uint32_t from = (b == startBlock && firstStart) ? startInst : 0;
        if (b == startBlock) firstStart = false;
        for (uint32_t i = from; i < block.instructions.size() && s.valid; ++i) {
            const il::Instruction& inst = block.instructions[i];
            const bool throws = il::canThrow(inst);
            const bool wasExposed = s.exposed;
            if (throws) {
                if (block.handler != il::kNoBlock) flow(block.handler, s);
                else if (s.exposed) excExposed = true;
            }
            bool usesAlias = false;
            for (il::ValueId v : inst.operands) if (isAlias(v)) usesAlias = true;
            bool handled = false;
            const il::ValueId a0 = inst.operands.empty() ? il::kNoValue : inst.operands[0];
            const bool recvAlias = a0 < nv && alias[a0];
            auto otherOperandsAlias = [&](size_t from) {
                for (size_t k = from; k < inst.operands.size(); ++k) if (isAlias(inst.operands[k])) return true;
                return false;
            };
            auto addKey = [&](uint32_t key, bool accessor) {
                if (solver_.keys().isIndex(key)) return;
                auto it = std::find_if(s.seq.begin(), s.seq.end(), [&](const SeqEntry& e) { return e.key == key; });
                if (it != s.seq.end()) {
                    if (it->accessor != accessor) inexact(s, "a key changes between data and accessor");
                    return;
                }
                if (s.exact) s.seq.push_back(SeqEntry{key, accessor});
            };
            auto followCall = [&](uint32_t g, const std::vector<il::ValueId>& calleeSeeds) {
                WalkOut sub = walk(g, 0, 0, calleeSeeds, {}, s, depth + 1);
                const bool resultUsed = inst.result != il::kNoValue && inst.result < nv && use[inst.result];
                s = sub.exit;
                if (sub.returnsAlias && resultUsed && !alias[inst.result]) {
                    s.exposed = true;
                    inexact(s, "a callee returns the object");
                }
            };
            // The parameters of `g` a dynamic-convention call feeds from
            // operands [first, end), with `self` the receiver operand.
            auto dynamicSeeds = [&](uint32_t g, il::ValueId self, size_t first,
                                    std::vector<il::ValueId>& out) -> bool {
                const il::Function& callee = mod.functions[g];
                size_t p = callee.needsEnv ? 1 : 0;
                if (callee.needsThis) {
                    if (isAlias(self)) out.push_back(static_cast<il::ValueId>(p));
                    ++p;
                }
                if (callee.needsArguments) {
                    if (otherOperandsAlias(first)) return false;
                    ++p;
                }
                const size_t fixed = callee.callerParamCount();
                for (size_t k = first; k < inst.operands.size(); ++k) {
                    if (!isAlias(inst.operands[k])) continue;
                    const size_t idx = k - first;
                    if (idx >= fixed) return false;
                    out.push_back(static_cast<il::ValueId>(p + idx));
                }
                return true;
            };
            auto single = [&](const AVal& callee, uint32_t& g) {
                const auto c = solver_.callees(callee);
                if (c.unknown || c.builtin || c.fns.size() != 1) return false;
                g = c.fns[0];
                return true;
            };

            if (usesAlias) {
                switch (inst.op) {
                    case il::Op::PropSet: case il::Op::DefineOwnAttr: case il::Op::MethodDef:
                    case il::Op::AccessorDef: {
                        if (!recvAlias) break;
                        handled = true;
                        const bool define = inst.op != il::Op::PropSet || il::isDefineWrite(inst);
                        if (!define && chainHasAccessor(inst.keyIndex)) {
                            s.exposed = true;
                            inexact(s, "a setter on the prototype chain runs");
                            break;
                        }
                        if (inst.op == il::Op::DefineOwnAttr &&
                            (static_cast<uint32_t>(inst.immI32) & BRONZE_ABI_DESC_HAS_VALUE) == 0) {
                            break;
                        }
                        addKey(inst.keyIndex, inst.op == il::Op::AccessorDef);
                        break;
                    }
                    case il::Op::PropGet:
                        if (a0 < nv && fnAlias[a0]) {
                            handled = true;
                            break;
                        }
                        if (!recvAlias) break;
                        handled = true;
                        if (chainHasAccessor(inst.keyIndex)) {
                            s.exposed = true;
                            inexact(s, "a getter on the prototype chain runs");
                        }
                        break;
                    case il::Op::ElemSet: case il::Op::ElemGet: {
                        if (!recvAlias || otherOperandsAlias(1)) break;
                        handled = true;
                        const AVal& idx = solver_.value(fnIdx, inst.operands[1]);
                        if (idx.onlyNumbers()) break;
                        if (inst.op == il::Op::ElemSet && idx.isStrConst()) {
                            if (chainHasAccessor(idx.strKey)) {
                                s.exposed = true;
                                inexact(s, "a setter on the prototype chain runs");
                            } else {
                                addKey(idx.strKey, false);
                            }
                            break;
                        }
                        if (inst.op == il::Op::ElemSet) inexact(s, "a write under a computed key");
                        break;
                    }
                    case il::Op::ClassExtend: case il::Op::MethodDefComputed: case il::Op::AccessorDefComputed:
                        if (a0 < nv && fnAlias[a0] && !otherOperandsAlias(1)) handled = true;
                        else if (recvAlias && inst.op != il::Op::ClassExtend) {
                            handled = true;
                            inexact(s, "a computed-key definition");
                        }
                        break;
                    case il::Op::InstanceOf: {
                        if (!recvAlias || otherOperandsAlias(1)) break;
                        const AVal& rhs = solver_.value(fnIdx, inst.operands[1]);
                        bool custom = rhs.topObj;
                        for (ObjId r : rhs.objs) if (!solver_.object(r).sym.empty()) custom = true;
                        if (!custom) handled = true;
                        break;
                    }
                    case il::Op::MethodCall: {
                        uint32_t g = 0;
                        if (recvAlias && chainHasAccessor(inst.keyIndex)) break;
                        const AVal fnv = solver_.queryProp(solver_.value(fnIdx, a0), inst.keyIndex);
                        if (!recvAlias) {
                            // Handed to a builtin that only keeps it: the
                            // object is reachable from here on, unchanged.
                            const auto c = solver_.callees(fnv);
                            if (!c.unknown && c.fns.empty() && c.builtin && c.builtinKeepsOnly) {
                                s.exposed = true;
                                handled = true;
                                break;
                            }
                        }
                        if (!single(fnv, g)) break;
                        std::vector<il::ValueId> cs;
                        if (!dynamicSeeds(g, a0, 1, cs)) break;
                        handled = true;
                        followCall(g, cs);
                        break;
                    }
                    case il::Op::DynamicCall: {
                        uint32_t g = 0;
                        if (isAlias(a0) || !single(solver_.value(fnIdx, a0), g)) break;
                        std::vector<il::ValueId> cs;
                        if (!dynamicSeeds(g, inst.operands.size() > 1 ? inst.operands[1] : il::kNoValue, 2, cs)) break;
                        handled = true;
                        followCall(g, cs);
                        break;
                    }
                    case il::Op::Construct: {
                        uint32_t g = 0;
                        if (isAlias(a0) || !single(solver_.value(fnIdx, a0), g)) break;
                        std::vector<il::ValueId> cs;
                        if (!dynamicSeeds(g, il::kNoValue, 1, cs)) break;
                        handled = true;
                        followCall(g, cs);
                        break;
                    }
                    case il::Op::SuperCall: {
                        uint32_t g = 0;
                        if (isAlias(a0) || otherOperandsAlias(2) || !single(solver_.value(fnIdx, a0), g)) break;
                        std::vector<il::ValueId> cs;
                        if (!dynamicSeeds(g, inst.operands[1], 2, cs)) break;
                        handled = true;
                        followCall(g, cs);
                        break;
                    }
                    case il::Op::Call: {
                        const il::Function& callee = mod.functions[inst.calleeIndex];
                        if (callee.isExternal || callee.needsArguments) break;
                        std::vector<il::ValueId> cs;
                        for (size_t k = 0; k < inst.operands.size(); ++k) {
                            if (isAlias(inst.operands[k])) cs.push_back(static_cast<il::ValueId>(k));
                        }
                        if (inst.callEnvHops != il::Instruction::kNoEnvHops && isAlias(inst.operands[0])) break;
                        handled = true;
                        followCall(inst.calleeIndex, cs);
                        break;
                    }
                    case il::Op::Jump: case il::Op::Ret:
                        handled = true;
                        break;
                    default:
                        if (isPureUse(inst.op) && (inst.op != il::Op::ElemSetTyped || !otherOperandsAlias(1))) {
                            handled = true;
                        }
                        break;
                }
                if (!handled) {
                    s.exposed = true;
                    inexact(s, std::string("the object is used by ") + il::opName(inst.op));
                }
            }
            // Code that runs while the object is reachable by others may add
            // keys. The object's own handled accesses run none; a followed
            // call's body was walked, so only the exposure it STARTED with
            // counts against the call itself.
            const bool isCall = inst.op == il::Op::MethodCall || inst.op == il::Op::DynamicCall ||
                                inst.op == il::Op::Construct || inst.op == il::Op::SuperCall ||
                                inst.op == il::Op::Call;
            if (throws) {
                if (handled && usesAlias && isCall) {
                    if (wasExposed) inexact(s, "code runs while the object is reachable");
                } else if (!(handled && usesAlias) && s.exposed) {
                    inexact(s, "code runs while the object is reachable");
                }
            }
            if (s.seq.size() > kMaxSeq) inexact(s, "too many keys");
            if (throws && block.handler != il::kNoBlock) flow(block.handler, s);

            // Successors.
            if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                auto edge = [&](const il::BlockTarget& t) {
                    if (t.block >= fn.blocks.size()) return;
                    const auto& params = fn.blocks[t.block].params;
                    for (size_t k = 0; k < t.args.size() && k < params.size(); ++k) {
                        // The object goes on under a name the walk does not
                        // follow: what runs from here on may change it.
                        if (isAlias(t.args[k]) && !(params[k].id < nv && alias[params[k].id])) s.exposed = true;
                    }
                    flow(t.block, s);
                };
                edge(inst.target);
                if (inst.op == il::Op::Branch) edge(inst.elseTarget);
            } else if (inst.op == il::Op::Ret) {
                join(out.exit, s);
                if (!inst.operands.empty() && isAlias(inst.operands[0])) ++aliasRets;
            } else if (inst.op == il::Op::Throw) {
                if (block.handler == il::kNoBlock && s.exposed) excExposed = true;
            }
        }
    }
    out.returnsAlias = aliasRets > 0;
    if (excExposed && out.exit.valid) out.exit.partial = true;
    stack_.pop_back();
    return out;
}

}  // namespace bronze::wpo
