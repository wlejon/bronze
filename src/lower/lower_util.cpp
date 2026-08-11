// Small shared helpers every other lowering unit leans on: the interned key
// table, block creation and instruction emission, the box/unbox coercions,
// and JS truthiness (docs/0005).

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

uint32_t Lowerer::getKeyConstantIndex(const std::string& key) {
    auto it = keyConstants_.find(key);
    if (it != keyConstants_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(keyConstants_.size());
    keyConstants_[key] = idx;
    return idx;
}

il::BlockId Lowerer::createBlock(il::Function& ilFn) {
    il::BlockId id = static_cast<il::BlockId>(ilFn.blocks.size());
    ilFn.blocks.push_back(il::Block{.id = id});
    return id;
}

void Lowerer::setCurrentBlock(size_t blockIdx) {
    currentBlockIdx_ = blockIdx;
}

void Lowerer::emitInst(il::Function& ilFn, const il::Instruction& inst) {
    if (currentBlockIdx_ >= ilFn.blocks.size()) {
        ilFn.blocks.push_back(il::Block{.id = static_cast<il::BlockId>(currentBlockIdx_)});
    }
    ilFn.blocks[currentBlockIdx_].instructions.push_back(inst);
}

bool Lowerer::currentBlockIsTerminated(const il::Function& ilFn) const {
    if (currentBlockIdx_ >= ilFn.blocks.size()) return false;
    const auto& insts = ilFn.blocks[currentBlockIdx_].instructions;
    return !insts.empty() && il::isTerminator(insts.back().op);
}

Lowerer::Value Lowerer::boxValueIfNeeded(Value val, il::Function& ilFn) {
    if (val.type == il::Type::Dynamic) return val;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::Box;
    inst.type = il::Type::Dynamic;
    inst.boxType = val.type;
    inst.result = res;
    inst.operands = {val.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

Lowerer::Value Lowerer::unboxValueIfNeeded(Value val, il::Type targetType, il::Function& ilFn) {
    if (val.type == targetType) return val;
    if (val.type == il::Type::Bool && targetType == il::Type::F64) {
        // ToNumber(bool) — route through the boxed form.
        val = boxValueIfNeeded(val, ilFn);
    }
    if (val.type == il::Type::Dynamic) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Unbox;
        inst.type = targetType;
        inst.result = res;
        inst.operands = {val.id};
        emitInst(ilFn, inst);
        return Value{res, targetType};
    }
    diags_.error(Span{}, std::string("cannot convert ") + il::typeName(val.type) + " to " +
                             il::typeName(targetType));
    return val;
}

// Combine the pre-read target value with the rhs of a compound
// assignment. `-=`, `*=`, `/=` and `%=` are ToNumber on both operands
// whatever came in, so they are always numeric. `+=` is not: JS `+`
// concatenates as soon as either side is a string, so it routes through
// the dynamic add unless inference *proved* the result is a Number
// (docs/0010 decision 3). Unproven is the dynamic path, never the
// numeric one — unboxing a string pointer as a double is a miscompile,
// not a pessimisation.
Lowerer::Value Lowerer::emitCompoundCombine(Value cur, Value rhs, ast::BinaryOp binOp,
                                            bool provenNumeric, il::Function& ilFn) {
    if (binOp == ast::BinaryOp::PlusAssign && !provenNumeric) {
        Value l = boxValueIfNeeded(cur, ilFn);
        Value r = boxValueIfNeeded(rhs, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Add;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {l.id, r.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }
    il::Op op = il::Op::Sub;
    switch (binOp) {
        case ast::BinaryOp::PlusAssign: op = il::Op::Add; break;
        case ast::BinaryOp::MinusAssign: op = il::Op::Sub; break;
        case ast::BinaryOp::StarAssign: op = il::Op::Mul; break;
        case ast::BinaryOp::SlashAssign: op = il::Op::Div; break;
        case ast::BinaryOp::PercentAssign: op = il::Op::Mod; break;
        default:
            diags_.error(Span{}, "unsupported compound assignment operator");
            return cur;
    }
    Value l = unboxValueIfNeeded(cur, il::Type::F64, ilFn);
    Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = op;
    inst.type = il::Type::F64;
    inst.result = res;
    inst.operands = {l.id, r.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

// Conform a value flowing along a branch edge to the target block
// parameter's type. Box into dynamic params; unbox out of dynamic
// values (runtime-checked); anything else is a type conflict.
Lowerer::Value Lowerer::coerceToType(Value val, il::Type target, il::Function& ilFn) {
    if (val.type == target) return val;
    if (target == il::Type::Dynamic) return boxValueIfNeeded(val, ilFn);
    return unboxValueIfNeeded(val, target, ilFn);
}

Lowerer::Value Lowerer::lowerCondition(const ast::Expr& expr, il::Function& ilFn) {
    auto valOpt = lowerExpr(expr, ilFn);
    if (!valOpt) return Value{il::kNoValue, il::Type::Bool};
    return lowerConditionFromVal(*valOpt, ilFn);
}

Lowerer::Value Lowerer::lowerConditionFromVal(Value val, il::Function& ilFn) {
    if (val.type == il::Type::Bool) return val;
    if (val.type == il::Type::F64) {
        il::ValueId zeroRes = ilFn.valueCount++;
        il::Instruction zeroInst;
        zeroInst.op = il::Op::ConstF64;
        zeroInst.type = il::Type::F64;
        zeroInst.result = zeroRes;
        zeroInst.immF64 = 0.0;
        emitInst(ilFn, zeroInst);

        il::ValueId cmpRes = ilFn.valueCount++;
        il::Instruction cmpInst;
        cmpInst.op = il::Op::CmpNe;
        cmpInst.type = il::Type::Bool;
        cmpInst.result = cmpRes;
        cmpInst.operands = {val.id, zeroRes};
        emitInst(ilFn, cmpInst);
        return Value{cmpRes, il::Type::Bool};
    }
    if (val.type == il::Type::I32) {
        il::ValueId zeroRes = ilFn.valueCount++;
        il::Instruction zeroInst;
        zeroInst.op = il::Op::ConstI32;
        zeroInst.type = il::Type::I32;
        zeroInst.result = zeroRes;
        zeroInst.immI32 = 0;
        emitInst(ilFn, zeroInst);

        il::ValueId cmpRes = ilFn.valueCount++;
        il::Instruction cmpInst;
        cmpInst.op = il::Op::CmpNe;
        cmpInst.type = il::Type::Bool;
        cmpInst.result = cmpRes;
        cmpInst.operands = {val.id, zeroRes};
        emitInst(ilFn, cmpInst);
        return Value{cmpRes, il::Type::Bool};
    }
    // Dynamic
    return unboxValueIfNeeded(val, il::Type::Bool, ilFn);
}

}  // namespace bronze::lower
