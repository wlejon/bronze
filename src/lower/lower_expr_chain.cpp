// Optional chains — `a?.b`, `a?.[k]`, `a?.()` — and the short circuit that
// is the whole of the feature (docs/0018 decision 4).
//
// The subtle part is not the `?.` itself but its REACH. ECMA-262 13.3.9
// short-circuits the OptionalChain, not the link: when the base of `a?.b` is
// nullish, `a?.b.c.d` produces undefined without evaluating `.c` or `.d`, and
// an argument written further down the chain never runs. So the check cannot
// be a local `is.nullish ? undefined : read` — it has to jump past everything
// the chain still had to do.
//
// Which is a join with several incoming edges: one per `?.` in the chain, plus
// the one where the whole chain succeeded. They disagree about the result and
// they can disagree about bindings too — `o?.[(hits = hits + 1, "x")]` assigns
// on one path and not the other — so the join's parameters cannot be sized
// until every edge is known. Hence the edges are COLLECTED while the chain is
// lowered and their jumps emitted afterwards, which is the one structural
// difference from `??`, whose join has exactly two.

#include <string>
#include <unordered_map>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

void Lowerer::emitChainShortCircuit(Value base, il::Function& ilFn) {
    Value boxed = boxValueIfNeeded(base, ilFn);
    il::ValueId nullish = ilFn.valueCount++;
    il::Instruction isNullish;
    isNullish.op = il::Op::IsNullish;
    isNullish.type = il::Type::Bool;
    isNullish.result = nullish;
    isNullish.operands = {boxed.id};
    emitInst(ilFn, isNullish);

    il::BlockId bShort = createBlock(ilFn);
    il::BlockId bCont = createBlock(ilFn);
    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {nullish};
    brInst.target = il::BlockTarget{.block = bShort, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bCont, .args = {}};
    emitInst(ilFn, brInst);

    // `null` and `undefined` and nothing else. Not truthiness: `0?.x` and
    // `""?.x` read the property, and a ToBoolean test here would be wrong for
    // four of JavaScript's seven falsy values.
    chainExits_.push_back(ChainExit{bShort, il::kNoValue, snapshotVarStates()});
    setCurrentBlock(bCont);
}

std::optional<Lowerer::Value> Lowerer::lowerChainBase(const ast::Expr& base, il::Function& ilFn,
                                                      bool onSpine) {
    spinePos_ = onSpine;
    auto val = lowerExpr(base, ilFn);
    spinePos_ = false;
    return val;
}

std::optional<Lowerer::Value> Lowerer::lowerOptionalChain(const ast::Expr& expr,
                                                          il::Function& ilFn) {
    return lowerChainJoin(
        [&] {
            spinePos_ = true;
            auto v = lowerExpr(expr, ilFn);
            spinePos_ = false;
            return v;
        },
        ChainMiss::Undefined, ilFn);
}

// The join itself, over whatever `body` lowers. Two callers: an optional
// chain read, and a `delete` whose operand is one — which differ only in
// what the short-circuit edges carry.
std::optional<Lowerer::Value> Lowerer::lowerChainJoin(
    const std::function<std::optional<Value>()>& body, ChainMiss miss, il::Function& ilFn) {
    // A chain nested inside another — `a?.b(c?.d)` — is lowered to completion
    // with its own edge list, so the inner one's short circuit reaches the
    // inner join and not the outer.
    auto savedExits = std::move(chainExits_);
    chainExits_.clear();

    auto valOpt = body();
    if (!valOpt) {
        chainExits_ = std::move(savedExits);
        return std::nullopt;
    }

    std::vector<ChainExit> exits = std::move(chainExits_);
    chainExits_ = std::move(savedExits);
    if (exits.empty()) {
        // Nothing short-circuited, so there is no join to build. Reached only
        // if the root walk and this one disagreed about where the chain is,
        // which would be a bug, not a program.
        return valOpt;
    }

    const Value result = boxValueIfNeeded(*valOpt, ilFn);
    ChainExit success{currentBlockIdx_, result.id, snapshotVarStates()};

    il::BlockId bJoin = createBlock(ilFn);
    il::ValueId resParam = ilFn.valueCount++;
    ilFn.blocks[bJoin].params.push_back({resParam, il::Type::Dynamic});

    // A binding needs a join parameter when the edges do not all carry the
    // same value for it. Declaration order, like every other join (docs/0005
    // decision 2): map iteration order is not deterministic output.
    std::vector<std::string> joinVars;
    for (const auto& name : getActiveVarsInDeclOrder()) {
        const il::ValueId here = success.state.at(name).valueId;
        for (const auto& exit : exits) {
            if (exit.state.at(name).valueId != here) {
                joinVars.push_back(name);
                break;
            }
        }
    }
    std::unordered_map<std::string, il::ValueId> joinParam;
    for (const auto& name : joinVars) {
        il::ValueId pId = ilFn.valueCount++;
        // Dynamic, not the edges' common type: the edges reach this join from
        // arbitrary points inside the chain and there is no proof they agree,
        // so every one of them boxes.
        ilFn.blocks[bJoin].params.push_back({pId, il::Type::Dynamic});
        joinParam[name] = pId;
    }

    auto emitEdge = [&](const ChainExit& edge) {
        setCurrentBlock(edge.blockIdx);
        restoreVarStates(edge.state);
        std::vector<il::ValueId> args;
        // kNoValue is the short circuit's answer, materialized on the edge
        // that produces it: `undefined` for a read, `true` for a `delete`,
        // which asks whether there was a Reference to remove rather than
        // what it held.
        if (edge.result != il::kNoValue) {
            args.push_back(edge.result);
        } else if (miss == ChainMiss::True) {
            il::ValueId t = ilFn.valueCount++;
            il::Instruction trueInst;
            trueInst.op = il::Op::ConstBool;
            trueInst.type = il::Type::Bool;
            trueInst.result = t;
            trueInst.immI32 = 1;
            emitInst(ilFn, trueInst);
            args.push_back(boxValueIfNeeded(Value{t, il::Type::Bool}, ilFn).id);
        } else {
            args.push_back(emitConstUndefined(ilFn));
        }
        for (const auto& name : joinVars) {
            Value v{edge.state.at(name).valueId, edge.state.at(name).type};
            args.push_back(coerceToType(v, il::Type::Dynamic, ilFn).id);
        }
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    };
    emitEdge(success);
    for (const auto& exit : exits) emitEdge(exit);

    setCurrentBlock(bJoin);
    restoreVarStates(success.state);
    for (const auto& name : joinVars) {
        auto& b = varBindings_[activeVarMap_[name]];
        b.valueId = joinParam.at(name);
        b.type = il::Type::Dynamic;
    }
    return Value{resParam, il::Type::Dynamic};
}

}  // namespace bronze::lower
