// Expressions that evaluate an operand on only some paths — ternary, &&, ||, ??
// — and the join machinery they share with the statement joins in
// lower_control.cpp.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

Lowerer::VarStateMap Lowerer::snapshotVarStates() const {
    VarStateMap snap;
    for (const auto& [name, idx] : activeVarMap_) {
        snap[name] = VarState{varBindings_[idx].valueId, varBindings_[idx].type};
    }
    return snap;
}

void Lowerer::restoreVarStates(const VarStateMap& snap) {
    for (const auto& [name, idx] : activeVarMap_) {
        auto it = snap.find(name);
        if (it != snap.end()) {
            varBindings_[idx].valueId = it->second.valueId;
            varBindings_[idx].type = it->second.type;
        }
    }
}

// Join parameters for every variable whose value differs between the
// two incoming states, appended after any params already on the join
// block (the expression's result param comes first by convention).
Lowerer::ExprJoin Lowerer::makeExprJoin(const VarStateMap& a, const VarStateMap& b,
                                        il::BlockId joinBlock, il::Function& ilFn) {
    ExprJoin join;
    for (const auto& name : getActiveVarsInDeclOrder()) {
        auto itA = a.find(name);
        auto itB = b.find(name);
        if (itA != a.end() && itB != b.end()) {
            if (itA->second.valueId != itB->second.valueId) join.vars.push_back(name);
        } else if (itA != a.end() || itB != b.end()) {
            join.vars.push_back(name);
        }
    }
    for (const auto& name : join.vars) {
        il::ValueId pId = ilFn.valueCount++;
        auto itA = a.find(name);
        auto itB = b.find(name);
        il::Type tA = itA != a.end() ? itA->second.type : il::Type::Dynamic;
        il::Type tB = itB != b.end() ? itB->second.type : il::Type::Dynamic;
        il::Type pType = (tA == tB) ? tA : il::Type::Dynamic;
        ilFn.blocks[joinBlock].params.push_back({pId, pType});
        join.paramId[name] = pId;
        join.paramType[name] = pType;
    }
    return join;
}

// Coerce (in the current block) one incoming state's values to the join
// param types and append them to that edge's argument list.
void Lowerer::appendExprJoinArgs(std::vector<il::ValueId>& args, const ExprJoin& join,
                                 const VarStateMap& state, il::Function& ilFn) {
    for (const auto& name : join.vars) {
        auto it = state.find(name);
        Value v = (it != state.end() && it->second.valueId != il::kNoValue)
                      ? Value{it->second.valueId, it->second.type}
                      : Value{emitConstUndefined(ilFn), il::Type::Dynamic};
        auto ptIt = join.paramType.find(name);
        il::Type targetType = ptIt != join.paramType.end() ? ptIt->second : il::Type::Dynamic;
        args.push_back(coerceToType(v, targetType, ilFn).id);
    }
}

void Lowerer::bindExprJoinParams(const ExprJoin& join) {
    for (const auto& name : join.vars) {
        auto& b = varBindings_[activeVarMap_[name]];
        b.valueId = join.paramId.at(name);
        b.type = join.paramType.at(name);
    }
}

std::optional<Lowerer::Value> Lowerer::lowerTernary(const ast::Ternary* tern,
                                                    il::Function& ilFn) {
    Value condVal = lowerCondition(*tern->condition, ilFn);
    if (condVal.id == il::kNoValue) return std::nullopt;

    il::BlockId bThen = createBlock(ilFn);
    il::BlockId bElse = createBlock(ilFn);
    il::BlockId bJoin = createBlock(ilFn);

    size_t entryBlockIdx = currentBlockIdx_;
    auto statePre = snapshotVarStates();

    setCurrentBlock(bThen);
    auto thenValOpt = lowerExpr(*tern->thenExpr, ilFn);
    if (!thenValOpt) return std::nullopt;
    auto stateThen = snapshotVarStates();
    bool thenReaches = !currentBlockIsTerminated(ilFn);
    size_t thenEndBlockIdx = currentBlockIdx_;

    restoreVarStates(statePre);
    setCurrentBlock(bElse);
    auto elseValOpt = lowerExpr(*tern->elseExpr, ilFn);
    if (!elseValOpt) return std::nullopt;
    auto stateElse = snapshotVarStates();
    bool elseReaches = !currentBlockIsTerminated(ilFn);
    size_t elseEndBlockIdx = currentBlockIdx_;

    il::Type joinType = il::Type::Dynamic;
    if (thenValOpt->type == elseValOpt->type) {
        joinType = thenValOpt->type;
    }

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {condVal.id};
    brInst.target = il::BlockTarget{.block = bThen, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bElse, .args = {}};
    ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

    il::ValueId resParamId = ilFn.valueCount++;
    ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
    ExprJoin join = makeExprJoin(stateThen, stateElse, bJoin, ilFn);

    auto emitArmEdge = [&](size_t endBlockIdx, Value v, const VarStateMap& state) {
        setCurrentBlock(endBlockIdx);
        if (joinType == il::Type::Dynamic) v = boxValueIfNeeded(v, ilFn);
        else if (v.type != joinType) v = unboxValueIfNeeded(v, joinType, ilFn);
        std::vector<il::ValueId> args{v.id};
        appendExprJoinArgs(args, join, state, ilFn);
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    };
    if (thenReaches) emitArmEdge(thenEndBlockIdx, *thenValOpt, stateThen);
    if (elseReaches) emitArmEdge(elseEndBlockIdx, *elseValOpt, stateElse);

    setCurrentBlock(bJoin);
    restoreVarStates(thenReaches || !elseReaches ? stateThen : stateElse);
    bindExprJoinParams(join);
    return Value{resParamId, joinType};
}

std::optional<Lowerer::Value> Lowerer::lowerLogical(const ast::Binary* bin, il::Function& ilFn) {
    auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
    if (!lhsOpt) return std::nullopt;
    Value lhsVal = *lhsOpt;

    Value lhsBool = lowerConditionFromVal(lhsVal, ilFn);

    il::BlockId bRhs = createBlock(ilFn);
    il::BlockId bJoin = createBlock(ilFn);
    size_t entryBlockIdx = currentBlockIdx_;
    auto stateLhs = snapshotVarStates();

    setCurrentBlock(bRhs);
    auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
    if (!rhsOpt) return std::nullopt;
    Value rhsVal = *rhsOpt;
    auto stateRhs = snapshotVarStates();
    bool rhsReaches = !currentBlockIsTerminated(ilFn);
    size_t rhsEndBlockIdx = currentBlockIdx_;

    il::Type joinType = (lhsVal.type == il::Type::F64 && rhsVal.type == il::Type::F64) ? il::Type::F64 : il::Type::Dynamic;

    il::ValueId resParamId = ilFn.valueCount++;
    ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
    ExprJoin join = makeExprJoin(stateLhs, stateRhs, bJoin, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {lhsBool.id};

    // The skip edge's conversions (lhs result and join-var
    // coercions) feed the entry block's branch, so they must be
    // emitted there, not in the rhs block.
    setCurrentBlock(entryBlockIdx);
    Value lhsBoxed = (joinType == il::Type::Dynamic) ? boxValueIfNeeded(lhsVal, ilFn) : unboxValueIfNeeded(lhsVal, joinType, ilFn);
    std::vector<il::ValueId> skipArgs{lhsBoxed.id};
    appendExprJoinArgs(skipArgs, join, stateLhs, ilFn);

    if (bin->op == ast::BinaryOp::LogicalAnd) {
        brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
        brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
    } else {
        brInst.target = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
        brInst.elseTarget = il::BlockTarget{.block = bRhs, .args = {}};
    }
    ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

    if (rhsReaches) {
        setCurrentBlock(rhsEndBlockIdx);
        Value rhsConv = (joinType == il::Type::Dynamic) ? boxValueIfNeeded(rhsVal, ilFn) : unboxValueIfNeeded(rhsVal, joinType, ilFn);
        std::vector<il::ValueId> args{rhsConv.id};
        appendExprJoinArgs(args, join, stateRhs, ilFn);
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    }

    setCurrentBlock(bJoin);
    restoreVarStates(stateLhs);
    bindExprJoinParams(join);
    return Value{resParamId, joinType};
}

std::optional<Lowerer::Value> Lowerer::lowerNullish(const ast::Binary* bin, il::Function& ilFn) {
    auto lhsOpt = lowerExpr(*bin->lhs, ilFn);
    if (!lhsOpt) return std::nullopt;
    Value lhsVal = *lhsOpt;

    if (lhsVal.type != il::Type::Dynamic) {
        // A typed value is statically never nullish; the rhs is
        // dead (and its side effects correctly never happen).
        return lhsVal;
    }
    il::ValueId isNullishRes = ilFn.valueCount++;
    il::Instruction nullishInst;
    nullishInst.op = il::Op::IsNullish;
    nullishInst.type = il::Type::Bool;
    nullishInst.result = isNullishRes;
    nullishInst.operands = {lhsVal.id};
    emitInst(ilFn, nullishInst);

    il::BlockId bRhs = createBlock(ilFn);
    il::BlockId bJoin = createBlock(ilFn);
    size_t entryBlockIdx = currentBlockIdx_;
    auto stateLhs = snapshotVarStates();

    setCurrentBlock(bRhs);
    auto rhsOpt = lowerExpr(*bin->rhs, ilFn);
    if (!rhsOpt) return std::nullopt;
    Value rhsVal = *rhsOpt;
    auto stateRhs = snapshotVarStates();
    bool rhsReaches = !currentBlockIsTerminated(ilFn);
    size_t rhsEndBlockIdx = currentBlockIdx_;

    il::Type joinType = il::Type::Dynamic;

    il::ValueId resParamId = ilFn.valueCount++;
    ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
    ExprJoin join = makeExprJoin(stateLhs, stateRhs, bJoin, ilFn);

    // Skip-edge coercions must dominate the entry branch.
    setCurrentBlock(entryBlockIdx);
    std::vector<il::ValueId> skipArgs{lhsVal.id};
    appendExprJoinArgs(skipArgs, join, stateLhs, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {isNullishRes};
    brInst.target = il::BlockTarget{.block = bRhs, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
    ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

    if (rhsReaches) {
        setCurrentBlock(rhsEndBlockIdx);
        Value rhsConv = boxValueIfNeeded(rhsVal, ilFn);
        std::vector<il::ValueId> args{rhsConv.id};
        appendExprJoinArgs(args, join, stateRhs, ilFn);
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    }

    setCurrentBlock(bJoin);
    restoreVarStates(stateLhs);
    bindExprJoinParams(join);
    return Value{resParamId, joinType};
}

}  // namespace bronze::lower
