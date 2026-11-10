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

// The entry block's `jump entry(args)` becomes a chain of receiver view tests
// followed by pre-body `is.number` tests: all passing leads into the fast
// copy, and the first that fails branches to the original slow entry.
void enterThrough(il::Function& fn, il::BlockId entry, il::BlockId copyEntry,
                  const std::vector<il::ValueId>& receivers,
                  const std::vector<int32_t>& receiverKinds,
                  const std::vector<il::ValueId>& guardedPreBody) {
    const il::Instruction jump = fn.blocks[0].instructions.back();
    fn.blocks[0].instructions.pop_back();
    const size_t totalTests = receivers.size() + guardedPreBody.size();
    std::vector<il::BlockId> tests{0};
    for (size_t t = 1; t < totalTests; ++t) {
        il::Block blk;
        blk.id = static_cast<il::BlockId>(fn.blocks.size());
        blk.copyClass = fn.blocks[0].copyClass;
        blk.copyRegion = fn.blocks[0].copyRegion;
        blk.handler = fn.blocks[0].handler;
        tests.push_back(blk.id);
        fn.blocks.push_back(std::move(blk));
    }
    for (size_t idx = 0; idx < totalTests; ++idx) {
        il::Instruction t = jump;
        t.type = il::Type::Bool;
        t.result = static_cast<il::ValueId>(fn.valueCount++);
        t.target = {};
        t.elseTarget = {};
        if (idx < receivers.size()) {
            t.op = il::Op::IsTypedArray;
            t.operands = {receivers[idx]};
            t.immI32 = receiverKinds[idx];
        } else {
            t.op = il::Op::IsNumber;
            t.operands = {guardedPreBody[idx - receivers.size()]};
            t.immI32 = 0;
        }
        il::Instruction br = jump;
        br.op = il::Op::Branch;
        br.operands = {t.result};
        br.target = idx + 1 < totalTests ? il::BlockTarget{tests[idx + 1], {}}
                                         : il::BlockTarget{copyEntry, jump.target.args};
        br.elseTarget = il::BlockTarget{entry, jump.target.args};
        fn.blocks[tests[idx]].instructions.push_back(std::move(t));
        fn.blocks[tests[idx]].instructions.push_back(std::move(br));
    }
}

il::BlockId copyBody(il::Function& fn, const il::Function& srcFn, const Cfg& cfg,
                     const TypedBodyPlan& plan,
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
                    f64Of[g] = emitRawUnbox(fn.blocks[partId].instructions, g, headSpan);
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
                if (orig.op == il::Op::Unbox && !orig.rawUnbox && !orig.nullishUnbox &&
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

                if (isBodyPromotableDynOp(srcFn, plan, orig)) {
                    bool allF64 = true;
                    std::vector<il::ValueId> fOps;
                    fOps.reserve(orig.operands.size());
                    for (il::ValueId opId : orig.operands) {
                        const il::ValueId f =
                            getF64(opId, b, static_cast<uint32_t>(i), outInsts, orig.span);
                        if (f == il::kNoValue) {
                            allF64 = false;
                            break;
                        }
                        fOps.push_back(f);
                    }
                    if (allF64) {
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
                        inst.rawUnbox = false;
                    }
                } else if (isBodyElemAccess(orig)) {
                    const auto rIt =
                        std::lower_bound(plan.receivers.begin(), plan.receivers.end(), orig.operands[0]);
                    if (rIt != plan.receivers.end() && *rIt == orig.operands[0]) {
                        const size_t rIdx = static_cast<size_t>(rIt - plan.receivers.begin());
                        const int32_t rKind = plan.receiverKinds[rIdx];
                        const il::ValueId fIdx =
                            getF64(orig.operands[1], b, static_cast<uint32_t>(i), outInsts, orig.span);
                        if (orig.op == il::Op::ElemGet) {
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

                std::vector<il::BlockId> testBlocks{partId};
                for (size_t k = 1; k < sp.guarded.size(); ++k) {
                    il::Block tb;
                    tb.id = static_cast<il::BlockId>(fn.blocks.size());
                    tb.handler = src.handler;
                    tb.copyClass = src.copyClass;
                    tb.copyRegion = src.copyRegion;
                    tb.cold = src.cold;
                    testBlocks.push_back(tb.id);
                    fn.blocks.push_back(std::move(tb));
                }
                for (size_t k = 0; k < sp.guarded.size(); ++k) {
                    il::Instruction isNum;
                    isNum.op = il::Op::IsNumber;
                    isNum.type = il::Type::Bool;
                    isNum.result = static_cast<il::ValueId>(fn.valueCount++);
                    isNum.operands = {mapV(sp.guarded[k])};
                    isNum.span = spSpan;

                    il::Instruction br;
                    br.op = il::Op::Branch;
                    br.type = il::Type::Void;
                    br.result = il::kNoValue;
                    br.operands = {isNum.result};
                    br.target = (k + 1 < sp.guarded.size())
                                    ? il::BlockTarget{testBlocks[k + 1], {}}
                                    : il::BlockTarget{fastPartId[b][seg + 1], {}};
                    br.elseTarget = il::BlockTarget{slowTailId[b][seg], fastTailArgs};
                    br.span = spSpan;

                    fn.blocks[testBlocks[k]].instructions.push_back(std::move(isNum));
                    fn.blocks[testBlocks[k]].instructions.push_back(std::move(br));
                }
            }
            cursor = limit;
        }
    }
    return fastPartId[plan.entry][0];
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

    const il::BlockId copyEntry = copyBody(fn, srcFn, cfg, plan, slowTailId, setNumbers);

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

    enterThrough(fn, plan.entry, copyEntry, plan.receivers, plan.receiverKinds, plan.guardedPreBody);
    dropDeadF64Boxes(fn);

    // Original slow body and its split tails sink behind their dominators and
    // are marked cold so register allocation never spills hot fast-copy values
    // across slow-path helper calls.
    std::vector<uint8_t> original(fn.blocks.size(), 0);
    for (il::BlockId b : plan.body) {
        original[b] = 1;
        for (il::BlockId tailId : slowTailId[b]) original[tailId] = 1;
    }
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
