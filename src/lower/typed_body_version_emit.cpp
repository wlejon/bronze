#include "lower/typed_body_version_plan.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "il/verifier.h"

namespace bronze::lower {

namespace {

void setRename(Rename& map, il::ValueId from, il::ValueId to) {
    for (auto& [k, v] : map) {
        if (k == from) {
            v = to;
            return;
        }
    }
    map.push_back({from, to});
}

il::BlockId appendEntryTestBlock(il::Function& fn) {
    il::Block blk;
    blk.id = static_cast<il::BlockId>(fn.blocks.size());
    blk.copyClass = fn.blocks[0].copyClass;
    blk.copyRegion = fn.blocks[0].copyRegion;
    blk.handler = fn.blocks[0].handler;
    const il::BlockId id = blk.id;
    fn.blocks.push_back(std::move(blk));
    return id;
}

void emitCondBranch(il::Function& fn, il::BlockId inBlk, il::ValueId cond, il::BlockTarget pass,
                    il::BlockTarget fail, Span span) {
    il::Instruction br;
    br.op = il::Op::Branch;
    br.type = il::Type::Void;
    br.result = il::kNoValue;
    br.operands = {cond};
    br.target = std::move(pass);
    br.elseTarget = std::move(fail);
    br.span = span;
    fn.blocks[inBlk].instructions.push_back(std::move(br));
}

// Emits shared pre-body `is.number` guards and read-probe base-offset integer
// range checks once ahead of version dispatch, populating `preBodyF64` with
// validated F64 base offsets that strictly dominate every fast copy.
il::BlockId emitPreBodyGuards(il::Function& fn, const il::Function& srcFn, const Cfg& cfg,
                              const TypedBodyPlan& plan, const il::Instruction& jump,
                              std::unordered_map<il::ValueId, il::ValueId>& preBodyF64) {
    const il::BlockTarget slowTarget{plan.entry, jump.target.args};
    il::BlockId cur = 0;

    for (il::ValueId g : plan.guardedPreBody) {
        il::Instruction isNum;
        isNum.op = il::Op::IsNumber;
        isNum.type = il::Type::Bool;
        isNum.result = static_cast<il::ValueId>(fn.valueCount++);
        isNum.operands = {g};
        isNum.span = jump.span;
        const il::ValueId cond = isNum.result;
        fn.blocks[cur].instructions.push_back(std::move(isNum));

        const il::BlockId nextBlk = appendEntryTestBlock(fn);
        emitCondBranch(fn, cur, cond, il::BlockTarget{nextBlk, {}}, slowTarget, jump.span);
        cur = nextBlk;
    }

    std::vector<std::pair<il::ValueId, int32_t>> baseLimits;
    for (const TypedBodyReadProbe& p : plan.readProbes) {
        if (p.baseVal == il::kNoValue) continue;
        auto it = std::find_if(baseLimits.begin(), baseLimits.end(),
                               [&](const auto& kv) { return kv.first == p.baseVal; });
        if (it == baseLimits.end()) {
            baseLimits.push_back({p.baseVal, p.maxOffset});
        } else if (p.maxOffset > it->second) {
            it->second = p.maxOffset;
        }
    }

    auto widenI32 = [&](il::BlockId intoBlk, il::ValueId i32Val) {
        il::Instruction z;
        z.op = il::Op::ConstI32;
        z.type = il::Type::I32;
        z.result = static_cast<il::ValueId>(fn.valueCount++);
        z.immI32 = 0;
        z.span = jump.span;
        il::Instruction w;
        w.op = il::Op::BitOr;
        w.type = il::Type::F64;
        w.result = static_cast<il::ValueId>(fn.valueCount++);
        w.operands = {i32Val, z.result};
        w.span = jump.span;
        const il::ValueId res = w.result;
        fn.blocks[intoBlk].instructions.push_back(std::move(z));
        fn.blocks[intoBlk].instructions.push_back(std::move(w));
        return res;
    };

    for (const auto& [baseVal, maxOff] : baseLimits) {
        il::ValueId fBase = il::kNoValue;
        if (baseVal < plan.types.size() && plan.types[baseVal] == il::Type::F64) {
            fBase = baseVal;
        } else if (baseVal < plan.types.size() && plan.types[baseVal] == il::Type::I32) {
            fBase = widenI32(cur, baseVal);
        } else if (baseVal < plan.defs.size() && plan.defs[baseVal].block != il::kNoBlock &&
                   plan.defs[baseVal].index != UINT32_MAX) {
            const il::Instruction& d =
                srcFn.blocks[plan.defs[baseVal].block].instructions[plan.defs[baseVal].index];
            if (d.op == il::Op::Box && d.operands.size() == 1) {
                if (d.boxType == il::Type::F64) {
                    fBase = d.operands[0];
                } else if (d.boxType == il::Type::I32) {
                    fBase = widenI32(cur, d.operands[0]);
                }
            }
        }
        if (fBase == il::kNoValue) {
            fBase = bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes, baseVal, plan.entry, 0);
        }
        if (fBase == il::kNoValue) {
            il::Instruction u;
            u.op = il::Op::Unbox;
            u.type = il::Type::F64;
            u.rawUnbox = true;
            u.result = static_cast<il::ValueId>(fn.valueCount++);
            u.operands = {baseVal};
            u.span = jump.span;
            fBase = u.result;
            fn.blocks[cur].instructions.push_back(std::move(u));
        }

        // 1. Reject negative offsets and NaN (ordered `>= 0.0` fails on NaN).
        il::Instruction c0;
        c0.op = il::Op::ConstF64;
        c0.type = il::Type::F64;
        c0.result = static_cast<il::ValueId>(fn.valueCount++);
        c0.immF64 = 0.0;
        c0.span = jump.span;
        il::Instruction ge0;
        ge0.op = il::Op::CmpGe;
        ge0.type = il::Type::Bool;
        ge0.result = static_cast<il::ValueId>(fn.valueCount++);
        ge0.operands = {fBase, c0.result};
        ge0.span = jump.span;
        const il::ValueId geCond = ge0.result;
        fn.blocks[cur].instructions.push_back(std::move(c0));
        fn.blocks[cur].instructions.push_back(std::move(ge0));
        const il::BlockId next1 = appendEntryTestBlock(fn);
        emitCondBranch(fn, cur, geCond, il::BlockTarget{next1, {}}, slowTarget, jump.span);
        cur = next1;

        // 2. Keep `base + maxOff` strictly inside signed 32-bit range.
        il::Instruction cLim;
        cLim.op = il::Op::ConstF64;
        cLim.type = il::Type::F64;
        cLim.result = static_cast<il::ValueId>(fn.valueCount++);
        cLim.immF64 = 2147483644.0 - static_cast<double>(maxOff);
        cLim.span = jump.span;
        il::Instruction ltLim;
        ltLim.op = il::Op::CmpLt;
        ltLim.type = il::Type::Bool;
        ltLim.result = static_cast<il::ValueId>(fn.valueCount++);
        ltLim.operands = {fBase, cLim.result};
        ltLim.span = jump.span;
        const il::ValueId ltCond = ltLim.result;
        fn.blocks[cur].instructions.push_back(std::move(cLim));
        fn.blocks[cur].instructions.push_back(std::move(ltLim));
        const il::BlockId next2 = appendEntryTestBlock(fn);
        emitCondBranch(fn, cur, ltCond, il::BlockTarget{next2, {}}, slowTarget, jump.span);
        cur = next2;

        // 3. Require exact integer equality via call-free I32 round-trip so
        // fractional / subnormal offsets take the generic body.
        il::Instruction toI;
        toI.op = il::Op::ToInt32;
        toI.type = il::Type::I32;
        toI.boxType = il::Type::F64;
        toI.rawUnbox = true;
        toI.result = static_cast<il::ValueId>(fn.valueCount++);
        toI.operands = {fBase};
        toI.span = jump.span;
        const il::ValueId iBase = toI.result;
        fn.blocks[cur].instructions.push_back(std::move(toI));
        const il::ValueId fRound = widenI32(cur, iBase);
        il::Instruction eqInt;
        eqInt.op = il::Op::CmpEq;
        eqInt.type = il::Type::Bool;
        eqInt.result = static_cast<il::ValueId>(fn.valueCount++);
        eqInt.operands = {fBase, fRound};
        eqInt.span = jump.span;
        const il::ValueId eqCond = eqInt.result;
        fn.blocks[cur].instructions.push_back(std::move(eqInt));
        const il::BlockId next3 = appendEntryTestBlock(fn);
        emitCondBranch(fn, cur, eqCond, il::BlockTarget{next3, {}}, slowTarget, jump.span);
        cur = next3;

        preBodyF64[baseVal] = fRound;
    }

    return cur;
}

void emitVersionDispatch(il::Function& fn, il::BlockId cur, const TypedBodyPlan& plan,
                         const std::vector<std::vector<int32_t>>& versions,
                         const std::vector<il::BlockId>& copyEntries, const il::Instruction& jump,
                         const std::unordered_map<il::ValueId, il::ValueId>& preBodyF64) {
    const il::BlockTarget slowTarget{plan.entry, jump.target.args};
    for (size_t v = 0; v < versions.size(); ++v) {
        const bool hasNextVersion = (v + 1 < versions.size());
        const il::BlockId nextVersionBlk = hasNextVersion ? appendEntryTestBlock(fn) : plan.entry;
        const il::BlockTarget missTarget =
            hasNextVersion ? il::BlockTarget{nextVersionBlk, {}} : slowTarget;

        for (size_t r = 0; r < plan.receivers.size(); ++r) {
            const bool lastCheck = (r + 1 == plan.receivers.size() && plan.readProbes.empty());
            const il::BlockId passBlk = lastCheck ? copyEntries[v] : appendEntryTestBlock(fn);
            const il::BlockTarget passTarget =
                lastCheck ? il::BlockTarget{passBlk, jump.target.args} : il::BlockTarget{passBlk, {}};

            il::Instruction isTa;
            isTa.op = il::Op::IsTypedArray;
            isTa.type = il::Type::Bool;
            isTa.result = static_cast<il::ValueId>(fn.valueCount++);
            isTa.operands = {plan.receivers[r]};
            isTa.immI32 = versions[v][r];
            isTa.span = jump.span;
            const il::ValueId cond = isTa.result;
            fn.blocks[cur].instructions.push_back(std::move(isTa));
            emitCondBranch(fn, cur, cond, passTarget, missTarget, jump.span);
            if (!lastCheck) cur = passBlk;
        }

        for (size_t pIdx = 0; pIdx < plan.readProbes.size(); ++pIdx) {
            const TypedBodyReadProbe& p = plan.readProbes[pIdx];
            const bool lastProbe = (pIdx + 1 == plan.readProbes.size());
            const il::BlockId passBlk = lastProbe ? copyEntries[v] : appendEntryTestBlock(fn);
            const il::BlockTarget passTarget =
                lastProbe ? il::BlockTarget{passBlk, jump.target.args} : il::BlockTarget{passBlk, {}};

            il::ValueId fMax = il::kNoValue;
            if (p.baseVal == il::kNoValue) {
                il::Instruction cOff;
                cOff.op = il::Op::ConstF64;
                cOff.type = il::Type::F64;
                cOff.result = static_cast<il::ValueId>(fn.valueCount++);
                cOff.immF64 = static_cast<double>(p.maxOffset);
                cOff.span = jump.span;
                fMax = cOff.result;
                fn.blocks[cur].instructions.push_back(std::move(cOff));
            } else if (p.maxOffset == 0) {
                fMax = preBodyF64.at(p.baseVal);
            } else {
                il::Instruction cOff;
                cOff.op = il::Op::ConstF64;
                cOff.type = il::Type::F64;
                cOff.result = static_cast<il::ValueId>(fn.valueCount++);
                cOff.immF64 = static_cast<double>(p.maxOffset);
                cOff.span = jump.span;
                il::Instruction sum;
                sum.op = il::Op::Add;
                sum.type = il::Type::F64;
                sum.result = static_cast<il::ValueId>(fn.valueCount++);
                sum.operands = {preBodyF64.at(p.baseVal), cOff.result};
                sum.span = jump.span;
                fMax = sum.result;
                fn.blocks[cur].instructions.push_back(std::move(cOff));
                fn.blocks[cur].instructions.push_back(std::move(sum));
            }

            // `ElemGetTyped` yields NaN when out of bounds or detached; testing
            // `elem == elem` certifies `base + maxOffset < view.length` (hence
            // every `base + k` in `0..maxOffset` is in bounds) and routes both
            // out-of-bounds (`undefined`) and rare stored NaNs to `slowTarget`.
            il::Instruction probeLoad;
            probeLoad.op = il::Op::ElemGetTyped;
            probeLoad.type = il::Type::F64;
            probeLoad.result = static_cast<il::ValueId>(fn.valueCount++);
            probeLoad.operands = {plan.receivers[p.recvIdx], fMax};
            probeLoad.immI32 = versions[v][p.recvIdx] | il::kElemFlagProvenIntegralIndex;
            probeLoad.span = jump.span;
            const il::ValueId loaded = probeLoad.result;
            fn.blocks[cur].instructions.push_back(std::move(probeLoad));

            il::Instruction notNan;
            notNan.op = il::Op::CmpEq;
            notNan.type = il::Type::Bool;
            notNan.result = static_cast<il::ValueId>(fn.valueCount++);
            notNan.operands = {loaded, loaded};
            notNan.span = jump.span;
            const il::ValueId okCond = notNan.result;
            fn.blocks[cur].instructions.push_back(std::move(notNan));
            emitCondBranch(fn, cur, okCond, passTarget, slowTarget, jump.span);
            if (!lastProbe) cur = passBlk;
        }

        if (hasNextVersion) cur = nextVersionBlk;
    }
}

il::BlockId copyBody(il::Function& fn, const il::Function& srcFn, const Cfg& cfg,
                     const TypedBodyPlan& plan, const std::vector<int32_t>& receiverKinds,
                     const std::unordered_map<il::ValueId, il::ValueId>& preBodyF64,
                     const std::vector<std::vector<il::BlockId>>& slowTailId,
                     const std::map<std::pair<il::BlockId, uint32_t>, il::ValueId>& setNumbers) {
    std::vector<std::vector<il::BlockId>> fastPartId(srcFn.blocks.size());
    for (il::BlockId b : plan.body) {
        const size_t parts = plan.splitsOf[b].size() + 1;
        fastPartId[b].resize(parts, il::kNoBlock);
        for (size_t seg = 0; seg < parts; ++seg) {
            il::Block blk;
            blk.id = static_cast<il::BlockId>(fn.blocks.size());
            blk.handler = srcFn.blocks[b].handler;
            blk.copyClass = srcFn.blocks[b].copyClass;
            blk.copyRegion = srcFn.blocks[b].copyRegion;
            blk.cold = srcFn.blocks[b].cold;
            fastPartId[b][seg] = blk.id;
            fn.blocks.push_back(std::move(blk));
        }
    }

    std::unordered_map<il::ValueId, il::ValueId> vmap;
    for (il::BlockId b : plan.body) {
        for (const auto& p : srcFn.blocks[b].params) {
            vmap[p.id] = static_cast<il::ValueId>(fn.valueCount++);
        }
        for (const auto& inst : srcFn.blocks[b].instructions) {
            if (inst.result != il::kNoValue) {
                vmap[inst.result] = static_cast<il::ValueId>(fn.valueCount++);
            }
        }
    }
    const auto mapV = [&](il::ValueId v) {
        const auto it = vmap.find(v);
        return it == vmap.end() ? v : it->second;
    };
    const auto mapT = [&](il::BlockTarget t) {
        if (t.block != il::kNoBlock && t.block < plan.in.size() && plan.in[t.block]) {
            t.block = fastPartId[t.block][0];
        }
        for (auto& a : t.args) a = mapV(a);
        return t;
    };

    // Maps an original SSA ValueId -> its unboxed F64 ValueId in the fast
    // copy. Entries are inserted only at program points that strictly dominate
    // every downstream use of that SSA value.
    std::unordered_map<il::ValueId, il::ValueId> f64Of;
    for (size_t b = 0; b < srcFn.blocks.size(); ++b) {
        if (b < plan.in.size() && plan.in[b]) continue;
        for (const auto& inst : srcFn.blocks[b].instructions) {
            if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 &&
                inst.operands.size() == 1 && inst.result != il::kNoValue) {
                f64Of[inst.result] = inst.operands[0];
            }
        }
    }
    for (size_t p = 0; p < srcFn.params.size(); ++p) {
        const auto v = static_cast<il::ValueId>(p);
        const il::ValueId u =
            bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes, v, plan.entry, 0);
        if (u != il::kNoValue) f64Of[v] = u;
    }
    for (size_t b = 0; b < srcFn.blocks.size(); ++b) {
        if (b < plan.in.size() && plan.in[b]) continue;
        for (const auto& p : srcFn.blocks[b].params) {
            const il::ValueId u =
                bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes, p.id, plan.entry, 0);
            if (u != il::kNoValue) f64Of[p.id] = u;
        }
        for (const auto& inst : srcFn.blocks[b].instructions) {
            if (inst.result == il::kNoValue) continue;
            const il::ValueId u = bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes,
                                                           inst.result, plan.entry, 0);
            if (u != il::kNoValue) f64Of[inst.result] = u;
        }
    }
    for (const auto& [k, fv] : preBodyF64) f64Of[k] = fv;

    auto getF64 = [&](il::ValueId origVal, il::BlockId b, uint32_t idx,
                      std::vector<il::Instruction>& emitInto, Span span) -> il::ValueId {
        if (origVal < plan.types.size() && plan.types[origVal] == il::Type::F64) return mapV(origVal);
        if (const auto it = f64Of.find(origVal); it != f64Of.end()) return it->second;
        const il::ValueId u = bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes, origVal, b, idx);
        if (u != il::kNoValue) return mapV(u);
        if (origVal < plan.defs.size() && plan.defs[origVal].block != il::kNoBlock &&
            plan.defs[origVal].index != UINT32_MAX) {
            const il::Instruction& d =
                srcFn.blocks[plan.defs[origVal].block].instructions[plan.defs[origVal].index];
            if (d.op == il::Op::Box && d.boxType == il::Type::I32 && d.operands.size() == 1) {
                il::Instruction zero;
                zero.op = il::Op::ConstI32;
                zero.type = il::Type::I32;
                zero.result = static_cast<il::ValueId>(fn.valueCount++);
                zero.immI32 = 0;
                zero.span = span;
                il::Instruction widen;
                widen.op = il::Op::BitOr;
                widen.type = il::Type::F64;
                widen.result = static_cast<il::ValueId>(fn.valueCount++);
                widen.operands = {mapV(d.operands[0]), zero.result};
                widen.span = span;
                const il::ValueId res = widen.result;
                emitInto.push_back(std::move(zero));
                emitInto.push_back(std::move(widen));
                return res;
            }
        }
        return il::kNoValue;
    };

    auto emitRawUnbox = [&](std::vector<il::Instruction>& into, il::ValueId dynId, Span span) {
        il::Instruction u;
        u.op = il::Op::Unbox;
        u.type = il::Type::F64;
        u.rawUnbox = true;
        u.result = static_cast<il::ValueId>(fn.valueCount++);
        u.operands = {dynId};
        u.span = span;
        const il::ValueId res = u.result;
        into.push_back(std::move(u));
        return res;
    };

    auto emitBoxF64 = [&](std::vector<il::Instruction>& into, il::ValueId resId, il::ValueId f64Id,
                          Span span) {
        il::Instruction box;
        box.op = il::Op::Box;
        box.type = il::Type::Dynamic;
        box.boxType = il::Type::F64;
        box.result = resId;
        box.operands = {f64Id};
        box.span = span;
        into.push_back(std::move(box));
    };

    for (il::BlockId b : plan.body) {
        const il::Block& src = srcFn.blocks[b];
        const auto& splits = plan.splitsOf[b];
        const Span headSpan = src.instructions.empty() ? Span{} : src.instructions.front().span;

        {
            auto& firstParams = fn.blocks[fastPartId[b][0]].params;
            for (const auto& p : src.params) firstParams.push_back({mapV(p.id), p.type});
        }

        // Propagate unboxed F64 values across internal join block parameters
        // (`slerpFlat`'s quaternion components) when every incoming edge in
        // `body` already carries an F64 form.
        if (b != plan.entry && !src.params.empty() && !cfg.preds[b].empty()) {
            for (size_t pi = 0; pi < src.params.size(); ++pi) {
                if (src.params[pi].type != il::Type::Dynamic) continue;
                bool allHaveF64 = true;
                for (il::BlockId p : cfg.preds[b]) {
                    const il::Instruction& pTerm = srcFn.blocks[p].instructions.back();
                    for (const il::BlockTarget* t : {&pTerm.target, &pTerm.elseTarget}) {
                        if (t->block != b) continue;
                        if (pi >= t->args.size() || f64Of.find(t->args[pi]) == f64Of.end()) {
                            allHaveF64 = false;
                        }
                    }
                }
                if (!allHaveF64) continue;
                const auto f64Param = static_cast<il::ValueId>(fn.valueCount++);
                fn.blocks[fastPartId[b][0]].params.push_back({f64Param, il::Type::F64});
                for (il::BlockId p : cfg.preds[b]) {
                    const il::Instruction& origTerm = srcFn.blocks[p].instructions.back();
                    il::Instruction& fastTerm = fn.blocks[fastPartId[p].back()].instructions.back();
                    if (origTerm.target.block == b && fastTerm.target.block == fastPartId[b][0]) {
                        fastTerm.target.args.push_back(f64Of.at(origTerm.target.args[pi]));
                    }
                    if (origTerm.elseTarget.block == b && fastTerm.elseTarget.block == fastPartId[b][0]) {
                        fastTerm.elseTarget.args.push_back(f64Of.at(origTerm.elseTarget.args[pi]));
                    }
                }
                f64Of[src.params[pi].id] = f64Param;
            }
        }

        size_t cursor = 0;
        for (size_t seg = 0; seg <= splits.size(); ++seg) {
            const il::BlockId partId = fastPartId[b][seg];
            const size_t limit = (seg < splits.size()) ? splits[seg].index : src.instructions.size();

            if (seg == 0 && cursor < limit && src.instructions[cursor].op == il::Op::ClosureEnv) {
                il::Instruction envInst = src.instructions[cursor];
                for (auto& o : envInst.operands) o = mapV(o);
                envInst.result = mapV(envInst.result);
                fn.blocks[partId].instructions.push_back(std::move(envInst));
                ++cursor;
            }

            if (b == plan.entry && seg == 0) {
                for (il::ValueId g : plan.guardedPreBody) {
                    if (f64Of.find(g) == f64Of.end()) {
                        f64Of[g] = emitRawUnbox(fn.blocks[partId].instructions, g, headSpan);
                    }
                }
            }
            if (seg > 0) {
                const Span spSpan = cursor < src.instructions.size() ? src.instructions[cursor].span : headSpan;
                for (il::ValueId v : splits[seg - 1].guarded) {
                    f64Of[v] = emitRawUnbox(fn.blocks[partId].instructions, mapV(v), spSpan);
                }
            }

            for (size_t i = cursor; i < limit; ++i) {
                const il::Instruction& orig = src.instructions[i];
                auto& outInsts = fn.blocks[partId].instructions;

                if (orig.op == il::Op::Box && orig.boxType == il::Type::F64 &&
                    orig.operands.size() == 1 && orig.result != il::kNoValue) {
                    f64Of[orig.result] = mapV(orig.operands[0]);
                }
                if (orig.op == il::Op::Unbox && !orig.nullishUnbox &&
                    orig.type == il::Type::F64 && orig.operands.size() == 1 &&
                    orig.result != il::kNoValue) {
                    const il::ValueId f =
                        getF64(orig.operands[0], b, static_cast<uint32_t>(i), outInsts, orig.span);
                    if (f != il::kNoValue) {
                        vmap[orig.result] = f;
                        f64Of[orig.result] = f;
                        continue;
                    }
                }
                if (orig.op == il::Op::Unbox && orig.type == il::Type::Bool &&
                    orig.operands.size() == 1 && orig.result != il::kNoValue) {
                    const il::ValueId f =
                        getF64(orig.operands[0], b, static_cast<uint32_t>(i), outInsts, orig.span);
                    if (f != il::kNoValue) {
                        il::Instruction truthy;
                        truthy.op = il::Op::NumTruthy;
                        truthy.type = il::Type::Bool;
                        truthy.result = mapV(orig.result);
                        truthy.operands = {f};
                        truthy.span = orig.span;
                        outInsts.push_back(std::move(truthy));
                        continue;
                    }
                }
                if (orig.op == il::Op::IsNumber && orig.operands.size() == 1 &&
                    orig.result != il::kNoValue && f64Of.find(orig.operands[0]) != f64Of.end()) {
                    il::Instruction t = orig;
                    t.op = il::Op::ConstBool;
                    t.operands.clear();
                    t.immI32 = 1;
                    t.result = mapV(orig.result);
                    outInsts.push_back(std::move(t));
                    continue;
                }
                if ((orig.op == il::Op::StrictEq || isBodyRelOp(orig.op)) &&
                    orig.operands.size() == 2 && orig.result != il::kNoValue) {
                    std::vector<il::Instruction> scratch;
                    const il::ValueId f0 =
                        getF64(orig.operands[0], b, static_cast<uint32_t>(i), scratch, orig.span);
                    const il::ValueId f1 =
                        (f0 != il::kNoValue)
                            ? getF64(orig.operands[1], b, static_cast<uint32_t>(i), scratch, orig.span)
                            : il::kNoValue;
                    if (f0 != il::kNoValue && f1 != il::kNoValue) {
                        for (auto& s : scratch) outInsts.push_back(std::move(s));
                        il::Instruction cmp;
                        cmp.op = promotedBodyCmpOp(orig.op);
                        cmp.type = il::Type::Bool;
                        cmp.result = mapV(orig.result);
                        cmp.operands = {f0, f1};
                        cmp.span = orig.span;
                        outInsts.push_back(std::move(cmp));
                        continue;
                    }
                }

                if (isBodyPromotableDynOp(srcFn, plan, orig)) {
                    bool allF64 = true;
                    std::vector<il::Instruction> scratch;
                    std::vector<il::ValueId> fOps;
                    fOps.reserve(orig.operands.size());
                    for (il::ValueId opId : orig.operands) {
                        const il::ValueId f =
                            getF64(opId, b, static_cast<uint32_t>(i), scratch, orig.span);
                        if (f == il::kNoValue) {
                            allF64 = false;
                            break;
                        }
                        fOps.push_back(f);
                    }
                    if (allF64) {
                        for (auto& s : scratch) outInsts.push_back(std::move(s));
                        il::Instruction typed = orig;
                        typed.type = il::Type::F64;
                        typed.result = static_cast<il::ValueId>(fn.valueCount++);
                        typed.numberHint = false;
                        if (isBodyBitwiseBinOp(orig.op)) {
                            std::vector<il::ValueId> i32Ops(2);
                            for (size_t k = 0; k < 2; ++k) {
                                il::Instruction toI32;
                                toI32.op = il::Op::ToInt32;
                                toI32.type = il::Type::I32;
                                toI32.boxType = il::Type::F64;
                                toI32.rawUnbox = false;
                                toI32.result = static_cast<il::ValueId>(fn.valueCount++);
                                toI32.operands = {fOps[k]};
                                toI32.span = orig.span;
                                i32Ops[k] = toI32.result;
                                outInsts.push_back(std::move(toI32));
                            }
                            typed.operands = std::move(i32Ops);
                        } else {
                            typed.operands = std::move(fOps);
                        }
                        const il::ValueId fRes = typed.result;
                        outInsts.push_back(std::move(typed));
                        f64Of[orig.result] = fRes;
                        emitBoxF64(outInsts, mapV(orig.result), fRes, orig.span);
                        continue;
                    }
                }

                il::Instruction inst = orig;
                for (auto& o : inst.operands) o = mapV(o);
                inst.target = mapT(orig.target);
                inst.elseTarget = mapT(orig.elseTarget);
                if (inst.result != il::kNoValue) inst.result = mapV(orig.result);

                // Feed call-free F64 operand into `ToInt32` when its Dynamic
                // source was guarded as a Number.
                if (orig.op == il::Op::ToInt32 && orig.operands.size() == 1) {
                    const il::ValueId f =
                        getF64(orig.operands[0], b, static_cast<uint32_t>(i), outInsts, orig.span);
                    if (f != il::kNoValue) {
                        inst.operands[0] = f;
                        inst.boxType = il::Type::F64;
                        inst.rawUnbox = false;
                    }
                } else if (isBodyElemAccess(orig)) {
                    const auto rIt =
                        std::lower_bound(plan.receivers.begin(), plan.receivers.end(), orig.operands[0]);
                    if (rIt != plan.receivers.end() && *rIt == orig.operands[0]) {
                        const size_t rIdx = static_cast<size_t>(rIt - plan.receivers.begin());
                        const int32_t rKind = receiverKinds[rIdx];
                        const il::ValueId fIdx =
                            getF64(orig.operands[1], b, static_cast<uint32_t>(i), outInsts, orig.span);
                        if (orig.op == il::Op::ElemGet) {
                            if (rKind != il::kTypedArrayWordView && fIdx != il::kNoValue &&
                                rIdx < plan.provenInBoundsRead.size() &&
                                plan.provenInBoundsRead[rIdx] != 0) {
                                il::Instruction ld;
                                ld.op = il::Op::ElemGetTyped;
                                ld.type = il::Type::F64;
                                ld.result = static_cast<il::ValueId>(fn.valueCount++);
                                ld.operands = {mapV(orig.operands[0]), fIdx};
                                ld.immI32 = rKind | il::kElemFlagProvenIntegralIndex |
                                            il::kElemFlagProvenInBounds;
                                ld.span = orig.span;
                                const il::ValueId fRes = ld.result;
                                outInsts.push_back(std::move(ld));
                                f64Of[orig.result] = fRes;
                                emitBoxF64(outInsts, mapV(orig.result), fRes, orig.span);
                                continue;
                            }
                            inst.immI32 |= il::kElemFlagWordViewReceiver;
                            if (fIdx != il::kNoValue) {
                                const auto bIdx = static_cast<il::ValueId>(fn.valueCount++);
                                emitBoxF64(outInsts, bIdx, fIdx, orig.span);
                                inst.operands[1] = bIdx;
                            }
                        } else {
                            il::ValueId fVal =
                                getF64(orig.operands[2], b, static_cast<uint32_t>(i), outInsts, orig.span);
                            if (fVal == il::kNoValue) {
                                if (const auto n = setNumbers.find({b, static_cast<uint32_t>(i)});
                                    n != setNumbers.end()) {
                                    fVal = mapV(n->second);
                                }
                            }
                            if (rKind != il::kTypedArrayWordView && fIdx != il::kNoValue &&
                                fVal != il::kNoValue) {
                                inst.op = il::Op::ElemSetTyped;
                                inst.type = il::Type::Void;
                                inst.result = il::kNoValue;
                                inst.operands = {mapV(orig.operands[0]), fIdx, fVal};
                                inst.immI32 = rKind;
                                inst.keyIndex = static_cast<uint32_t>(il::writeMode(orig));
                            } else {
                                inst.immI32 |= il::kElemFlagWordViewReceiver;
                                if (fIdx != il::kNoValue) {
                                    const auto bIdx = static_cast<il::ValueId>(fn.valueCount++);
                                    emitBoxF64(outInsts, bIdx, fIdx, orig.span);
                                    inst.operands[1] = bIdx;
                                }
                                if (fVal != il::kNoValue) {
                                    const auto bVal = static_cast<il::ValueId>(fn.valueCount++);
                                    emitBoxF64(outInsts, bVal, fVal, orig.span);
                                    inst.operands[2] = bVal;
                                }
                            }
                        }
                    }
                }
                outInsts.push_back(std::move(inst));
            }

            if (seg < splits.size()) {
                const TypedBodyGuardSplit& sp = splits[seg];
                const Span spSpan = limit < src.instructions.size() ? src.instructions[limit].span : headSpan;
                std::vector<il::ValueId> fastTailArgs;
                fastTailArgs.reserve(sp.params.size());
                for (il::ValueId v : sp.params) fastTailArgs.push_back(mapV(v));

                // One test a block: the Numbers, then the read's pending
                // guard, so that it stands right in front of its read; the
                // last one passes into the next part and every one fails into
                // the original body at this point.
                const size_t tests = sp.guarded.size() + (sp.shapeOf != il::kNoValue ? 1 : 0);
                std::vector<il::BlockId> testBlocks{partId};
                for (size_t k = 1; k < tests; ++k) {
                    il::Block tb;
                    tb.id = static_cast<il::BlockId>(fn.blocks.size());
                    tb.handler = src.handler;
                    tb.copyClass = src.copyClass;
                    tb.copyRegion = src.copyRegion;
                    tb.cold = src.cold;
                    testBlocks.push_back(tb.id);
                    fn.blocks.push_back(std::move(tb));
                }
                for (size_t k = 0; k < tests; ++k) {
                    il::Instruction test;
                    test.type = il::Type::Bool;
                    test.result = static_cast<il::ValueId>(fn.valueCount++);
                    test.span = spSpan;
                    if (k < sp.guarded.size()) {
                        test.op = il::Op::IsNumber;
                        test.operands = {mapV(sp.guarded[k])};
                    } else {
                        // No claim yet: resolveReadExits gives it the read's.
                        test.op = il::Op::IsShape;
                        test.operands = {mapV(sp.shapeOf)};
                    }

                    il::Instruction br;
                    br.op = il::Op::Branch;
                    br.type = il::Type::Void;
                    br.result = il::kNoValue;
                    br.operands = {test.result};
                    br.target = (k + 1 < tests) ? il::BlockTarget{testBlocks[k + 1], {}}
                                                : il::BlockTarget{fastPartId[b][seg + 1], {}};
                    br.elseTarget = il::BlockTarget{slowTailId[b][seg], fastTailArgs};
                    br.span = spSpan;

                    fn.blocks[testBlocks[k]].instructions.push_back(std::move(test));
                    fn.blocks[testBlocks[k]].instructions.push_back(std::move(br));
                }
            }
            cursor = limit;
        }
    }
    return fastPartId[plan.entry][0];
}

// Folds conditional branches on `const.bool` (e.g. `is.number` guards folded
// after `ElemGetTyped` promotion) into direct jumps so `pruneUnreachable` drops
// orphaned trampoline blocks and their `box.f64` operands.
void foldConstBranches(il::Function& fn) {
    std::unordered_map<il::ValueId, uint8_t> constBool;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::ConstBool && inst.result != il::kNoValue) {
                constBool[inst.result] = inst.immI32 != 0 ? 1 : 0;
            }
        }
    }
    if (constBool.empty()) return;
    for (auto& blk : fn.blocks) {
        if (blk.instructions.empty()) continue;
        il::Instruction& term = blk.instructions.back();
        if (term.op != il::Op::Branch || term.operands.size() != 1) continue;
        const auto it = constBool.find(term.operands[0]);
        if (it == constBool.end()) continue;
        il::BlockTarget taken = it->second ? std::move(term.target) : std::move(term.elseTarget);
        term.op = il::Op::Jump;
        term.operands.clear();
        term.target = std::move(taken);
        term.elseTarget = {};
    }
}

// Removes unused Dynamic block parameters on internal fast-copy join blocks so
// `box.f64` values passed on incoming edges become dead for `dropDeadF64Boxes`.
void dropUnusedFastJoinParams(il::Function& fn, const std::vector<uint8_t>& slowOriginal) {
    std::vector<uint8_t> isHandler(fn.blocks.size(), 0);
    for (const auto& blk : fn.blocks) {
        if (blk.handler < isHandler.size()) isHandler[blk.handler] = 1;
    }
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<uint8_t> used(fn.valueCount, 0);
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                for (il::ValueId v : inst.operands) {
                    if (v < used.size()) used[v] = 1;
                }
                for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                    for (il::ValueId v : t->args) {
                        if (v < used.size()) used[v] = 1;
                    }
                }
            }
        }
        for (il::BlockId b = 1; b < fn.blocks.size(); ++b) {
            if ((b < slowOriginal.size() && slowOriginal[b]) || isHandler[b] ||
                fn.blocks[b].copyClass == il::CopyClass::Slow || fn.blocks[b].params.empty()) {
                continue;
            }
            auto& params = fn.blocks[b].params;
            std::vector<uint8_t> keep(params.size(), 1);
            bool anyDead = false;
            for (size_t pi = 0; pi < params.size(); ++pi) {
                if (params[pi].id < used.size() && !used[params[pi].id]) {
                    keep[pi] = 0;
                    anyDead = true;
                }
            }
            if (!anyDead) continue;
            changed = true;
            std::vector<il::BlockParam> keptParams;
            keptParams.reserve(params.size());
            for (size_t pi = 0; pi < params.size(); ++pi) {
                if (keep[pi]) keptParams.push_back(params[pi]);
            }
            params = std::move(keptParams);
            for (auto& pred : fn.blocks) {
                if (pred.instructions.empty()) continue;
                il::Instruction& term = pred.instructions.back();
                for (il::BlockTarget* t : {&term.target, &term.elseTarget}) {
                    if (t->block != b) continue;
                    std::vector<il::ValueId> keptArgs;
                    keptArgs.reserve(t->args.size());
                    for (size_t pi = 0; pi < t->args.size() && pi < keep.size(); ++pi) {
                        if (keep[pi]) keptArgs.push_back(t->args[pi]);
                    }
                    t->args = std::move(keptArgs);
                }
            }
        }
    }
}

}  // namespace

bool emitTypedBodyVersion(il::Function& fn, const Cfg& cfg, TypedBodyPlan& plan,
                          std::vector<il::BlockId>& coldOut) {
    const auto definedIn = [&](il::ValueId v) {
        return v < plan.defs.size() && plan.defs[v].block != il::kNoBlock &&
               plan.in[plan.defs[v].block];
    };

    const il::Function srcFn = fn;
    const std::vector<std::vector<il::ValueId>> liveIn = computeLiveIn(srcFn, cfg);

    std::vector<std::vector<il::BlockId>> slowTailId(srcFn.blocks.size());
    for (il::BlockId b : plan.body) {
        auto& splits = plan.splitsOf[b];
        slowTailId[b].resize(splits.size(), il::kNoBlock);
        for (size_t j = 0; j < splits.size(); ++j) {
            for (il::ValueId v : liveBefore(srcFn, cfg, liveIn, b, splits[j].index)) {
                if (!definedIn(v)) continue;
                splits[j].params.push_back(v);
                splits[j].paramIds.push_back(static_cast<il::ValueId>(fn.valueCount++));
            }
            il::Block tail;
            tail.id = static_cast<il::BlockId>(fn.blocks.size());
            tail.handler = srcFn.blocks[b].handler;
            tail.copyClass = srcFn.blocks[b].copyClass;
            tail.copyRegion = srcFn.blocks[b].copyRegion;
            tail.cold = srcFn.blocks[b].cold;
            slowTailId[b][j] = tail.id;
            fn.blocks.push_back(std::move(tail));
        }
    }

    std::map<std::pair<il::BlockId, uint32_t>, il::ValueId> setNumbers;
    for (il::BlockId b : plan.body) {
        const auto& insts = srcFn.blocks[b].instructions;
        for (uint32_t i = 0; i < insts.size(); ++i) {
            const il::Instruction& inst = insts[i];
            if (inst.op != il::Op::ElemSet || !isBodyElemAccess(inst) || inst.operands.size() < 3 ||
                !std::binary_search(plan.receivers.begin(), plan.receivers.end(), inst.operands[0])) {
                continue;
            }
            const il::ValueId u =
                bodyDominatingRawUnboxAt(cfg, plan.defs, plan.rawUnboxes, inst.operands[2], b, i);
            if (u != il::kNoValue) setNumbers[{b, i}] = u;
        }
    }

    const il::Instruction jump = fn.blocks[0].instructions.back();
    fn.blocks[0].instructions.pop_back();
    std::unordered_map<il::ValueId, il::ValueId> preBodyF64;
    const il::BlockId dispatchHead = emitPreBodyGuards(fn, srcFn, cfg, plan, jump, preBodyF64);

    const std::vector<std::vector<int32_t>> versions =
        plan.versionKinds.empty() ? std::vector<std::vector<int32_t>>{plan.receiverKinds}
                                  : plan.versionKinds;
    std::vector<il::BlockId> copyEntries;
    copyEntries.reserve(versions.size());
    for (const auto& rKinds : versions) {
        copyEntries.push_back(
            copyBody(fn, srcFn, cfg, plan, rKinds, preBodyF64, slowTailId, setNumbers));
    }

    // Rewrite slow `body` blocks to thread any `splitsOf` tails and rename
    // dominated uses to the nearest dominating split's parameters.
    if (plan.totalSplits > 0) {
        const auto initialSlowNames = [&](il::BlockId u) {
            std::vector<il::BlockId> domChain;
            for (il::BlockId d = cfg.idom[u]; d != il::kNoBlock && d < plan.in.size() && plan.in[d];
                 d = cfg.idom[d]) {
                domChain.push_back(d);
            }
            Rename names;
            for (auto it = domChain.rbegin(); it != domChain.rend(); ++it) {
                for (const TypedBodyGuardSplit& sp : plan.splitsOf[*it]) {
                    for (size_t i = 0; i < sp.params.size(); ++i) {
                        setRename(names, sp.params[i], sp.paramIds[i]);
                    }
                }
            }
            return names;
        };

        for (il::BlockId b : plan.body) {
            const il::Block& source = srcFn.blocks[b];
            const auto& splits = plan.splitsOf[b];
            Rename names = initialSlowNames(b);

            auto emitSlowRange = [&](il::BlockId intoId, size_t from, size_t to) {
                auto& dstInsts = fn.blocks[intoId].instructions;
                for (size_t k = from; k < to; ++k) {
                    il::Instruction inst = source.instructions[k];
                    rewriteUses(inst, [&](il::ValueId v) { return renamed(names, v); });
                    dstInsts.push_back(std::move(inst));
                }
            };

            fn.blocks[b].instructions.clear();
            size_t cursor = 0;
            il::BlockId curId = b;
            for (size_t j = 0; j < splits.size(); ++j) {
                emitSlowRange(curId, cursor, splits[j].index);
                il::Instruction jmp;
                jmp.op = il::Op::Jump;
                jmp.type = il::Type::Void;
                jmp.result = il::kNoValue;
                jmp.target.block = slowTailId[b][j];
                for (il::ValueId v : splits[j].params) {
                    jmp.target.args.push_back(renamed(names, v));
                }
                if (splits[j].index < source.instructions.size()) {
                    jmp.span = source.instructions[splits[j].index].span;
                }
                fn.blocks[curId].instructions.push_back(std::move(jmp));

                const il::BlockId tailId = slowTailId[b][j];
                for (size_t i = 0; i < splits[j].params.size(); ++i) {
                    fn.blocks[tailId].params.push_back(
                        il::BlockParam{splits[j].paramIds[i], plan.types[splits[j].params[i]]});
                    setRename(names, splits[j].params[i], splits[j].paramIds[i]);
                }
                cursor = splits[j].index;
                curId = tailId;
            }
            emitSlowRange(curId, cursor, source.instructions.size());
        }
    }

    emitVersionDispatch(fn, dispatchHead, plan, versions, copyEntries, jump, preBodyF64);

    // Original slow body and its split tails sink behind their dominators and
    // are marked cold so register allocation never spills hot fast-copy values
    // across slow-path helper calls.
    std::vector<uint8_t> original(fn.blocks.size(), 0);
    for (il::BlockId b : plan.body) {
        original[b] = 1;
        for (il::BlockId tailId : slowTailId[b]) original[tailId] = 1;
    }
    foldConstBranches(fn);
    pruneUnreachable(fn, original);
    dropUnusedFastJoinParams(fn, original);
    dropDeadF64Boxes(fn);
    orderAfterDominators(fn, original);
    DiagnosticSink verifyDiags;
    if (!dominanceHolds(fn) || !il::verifyFunction(fn, verifyDiags)) return false;
    for (il::BlockId b = 0; b < original.size(); ++b) {
        if (original[b]) {
            fn.blocks[b].cold = true;
            coldOut.push_back(b);
        }
    }
    return true;
}

}  // namespace bronze::lower
