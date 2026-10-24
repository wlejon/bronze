// Integral-index and loop-in-bounds proofs for `elem.get.typed` / `elem.set.typed`,
// plus the lowered-IL loop verification pass that strips `kElemFlagProvenInBounds`
// whenever any instruction in the bounding natural loop can invoke user JS or
// detach/resize an ArrayBuffer.

#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "ast/query_walk.h"
#include "lower/guard_region_cfg.h"
#include "lower/inline_small.h"
#include "lower/loop_prop_analysis.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

class ConstDeclFinder final : public ast::detail::IdentVisitor {
public:
    explicit ConstDeclFinder(const std::string& target) : target_(target) {}

    int count = 0;
    const ast::VarDecl* decl = nullptr;

    void visit(const ast::VarDecl& v) override {
        if (v.pattern) {
            for (const auto& n : ast::patternBoundNames(*v.pattern)) {
                if (n == target_) ++count;
            }
        } else if (v.name == target_) {
            ++count;
            decl = &v;
        }
        ast::detail::IdentVisitor::visit(v);
    }
    void visit(const ast::FunctionDecl& f) override {
        if (f.name == target_) ++count;
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ClassDecl& c) override {
        if (c.name == target_) ++count;
        ast::detail::IdentVisitor::visit(c);
    }

private:
    const std::string& target_;
};

bool isSafeIntegerLiteral(const ast::Expr& e, int64_t* outVal = nullptr) {
    if (const auto* n = dynamic_cast<const ast::NumberLit*>(&e)) {
        if (!std::isfinite(n->value) || std::floor(n->value) != n->value ||
            n->value < -2147483648.0 || n->value > 2147483647.0) {
            return false;
        }
        if (outVal) *outVal = static_cast<int64_t>(n->value);
        return true;
    }
    if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) {
        if ((u->op == ast::UnaryOp::Negate || u->op == ast::UnaryOp::Posate) && u->operand) {
            int64_t inner = 0;
            if (!isSafeIntegerLiteral(*u->operand, &inner)) return false;
            if (u->op == ast::UnaryOp::Negate) {
                if (inner == 0) return false;  // -0
                inner = -inner;
            }
            if (outVal) *outVal = inner;
            return true;
        }
    }
    return false;
}

class EnclosingForFinder final : public ast::detail::IdentVisitor {
public:
    using ast::detail::IdentVisitor::visit;
    explicit EnclosingForFinder(const ast::IndexAccess& target) : target_(&target) {}

    std::vector<const ast::ForStmt*> enclosing;
    std::vector<const ast::BlockStmt*> enclosingBlocks;

    void visit(const ast::IndexAccess& ia) override {
        if (&ia == target_) {
            enclosing = stack_;
            enclosingBlocks = blockStack_;
        }
        ast::detail::IdentVisitor::visit(ia);
    }
    void visit(const ast::BlockStmt& b) override {
        blockStack_.push_back(&b);
        for (const auto& s : b.stmts) s->accept(*this);
        blockStack_.pop_back();
    }
    void visit(const ast::ForStmt& f) override {
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        stack_.push_back(&f);
        for (const auto& s : f.body) s->accept(*this);
        stack_.pop_back();
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::FunctionExpr&) override {}

private:
    const ast::IndexAccess* target_;
    std::vector<const ast::ForStmt*> stack_;
    std::vector<const ast::BlockStmt*> blockStack_;
};

class LoopBodySafetyScan final : public ast::detail::IdentVisitor {
public:
    using ast::detail::IdentVisitor::visit;
    LoopBodySafetyScan(const std::string& ivName, const std::string& arrName,
                       std::function<bool(const ast::Expr&)> isProvenNumeric,
                       std::function<bool(const ast::IndexAccess&)> isTypedElem)
        : ivName_(ivName),
          arrName_(arrName),
          isProvenNumeric_(std::move(isProvenNumeric)),
          isTypedElem_(std::move(isTypedElem)) {}

    bool ivMutatedOrShadowed = false;
    bool arrMutatedOrShadowed = false;
    bool hasBufferHazard = false;

    void checkBoundName(const std::string& n) {
        if (n == ivName_) ivMutatedOrShadowed = true;
        if (!arrName_.empty() && n == arrName_) arrMutatedOrShadowed = true;
    }

    void visit(const ast::VarDecl& v) override {
        if (v.pattern) {
            for (const auto& n : ast::patternBoundNames(*v.pattern)) checkBoundName(n);
            hasBufferHazard = true;
        } else {
            checkBoundName(v.name);
        }
        ast::detail::IdentVisitor::visit(v);
    }
    void visit(const ast::FunctionDecl& f) override { checkBoundName(f.name); }
    void visit(const ast::ClassDecl& c) override {
        checkBoundName(c.name);
        hasBufferHazard = true;
    }
    void visit(const ast::Unary& u) override {
        if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PostInc ||
            u.op == ast::UnaryOp::PreDec || u.op == ast::UnaryOp::PostDec ||
            u.op == ast::UnaryOp::Delete) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(u.operand.get())) {
                checkBoundName(id->name);
            }
            if (u.op == ast::UnaryOp::Delete) hasBufferHazard = true;
        }
        if (u.op == ast::UnaryOp::Posate || u.op == ast::UnaryOp::Negate ||
            u.op == ast::UnaryOp::BitNot || u.op == ast::UnaryOp::PreInc ||
            u.op == ast::UnaryOp::PostInc || u.op == ast::UnaryOp::PreDec ||
            u.op == ast::UnaryOp::PostDec) {
            if (!u.operand || (isProvenNumeric_ && !isProvenNumeric_(*u.operand))) {
                hasBufferHazard = true;
            }
        }
        ast::detail::IdentVisitor::visit(u);
    }
    void visit(const ast::Binary& b) override {
        if (ast::isAssignOp(b.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get())) {
                checkBoundName(id->name);
                if (b.op != ast::BinaryOp::Assign && isProvenNumeric_ &&
                    (!isProvenNumeric_(*b.lhs) || !b.rhs || !isProvenNumeric_(*b.rhs))) {
                    hasBufferHazard = true;
                }
            } else if (const auto* ia = dynamic_cast<const ast::IndexAccess*>(b.lhs.get())) {
                if ((isTypedElem_ && !isTypedElem_(*ia)) ||
                    (isProvenNumeric_ && (!b.rhs || !isProvenNumeric_(*b.rhs)))) {
                    hasBufferHazard = true;
                }
            } else {
                hasBufferHazard = true;
            }
        } else if (b.op != ast::BinaryOp::LogicalAnd && b.op != ast::BinaryOp::LogicalOr &&
                   b.op != ast::BinaryOp::NullishCoalescing && b.op != ast::BinaryOp::StrictEq &&
                   b.op != ast::BinaryOp::StrictNe && b.op != ast::BinaryOp::Comma) {
            if (isProvenNumeric_ &&
                (!b.lhs || !b.rhs || !isProvenNumeric_(*b.lhs) || !isProvenNumeric_(*b.rhs))) {
                hasBufferHazard = true;
            }
        }
        ast::detail::IdentVisitor::visit(b);
    }
    void visit(const ast::IndexAccess& ia) override {
        if (isTypedElem_ && !isTypedElem_(ia)) {
            hasBufferHazard = true;
        }
        ast::detail::IdentVisitor::visit(ia);
    }
    void visit(const ast::TemplateLit& t) override {
        hasBufferHazard = true;
        ast::detail::IdentVisitor::visit(t);
    }
    void visit(const ast::DestructuringAssign& d) override {
        if (d.pattern) {
            for (const auto& n : ast::patternBoundNames(*d.pattern)) checkBoundName(n);
        }
        hasBufferHazard = true;
        ast::detail::IdentVisitor::visit(d);
    }
    void visit(const ast::ForInStmt& f) override {
        if (f.pattern) {
            for (const auto& n : ast::patternBoundNames(*f.pattern)) checkBoundName(n);
        }
        hasBufferHazard = true;
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ForOfStmt& f) override {
        if (f.pattern) {
            for (const auto& n : ast::patternBoundNames(*f.pattern)) checkBoundName(n);
        }
        hasBufferHazard = true;
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::Call&) override { hasBufferHazard = true; }
    void visit(const ast::NewExpr&) override { hasBufferHazard = true; }
    void visit(const ast::YieldExpr&) override { hasBufferHazard = true; }
    void visit(const ast::TaggedTemplate&) override { hasBufferHazard = true; }
    void visit(const ast::SuperCall&) override { hasBufferHazard = true; }
    void visit(const ast::MemberAccess& m) override {
        if (const auto* id = dynamic_cast<const ast::Ident*>(m.object.get())) {
            if (!arrName_.empty() && id->name == arrName_ && m.property == "length" &&
                !m.optional) {
                return;
            }
        }
        hasBufferHazard = true;
        ast::detail::IdentVisitor::visit(m);
    }

private:
    std::string ivName_;
    std::string arrName_;
    std::function<bool(const ast::Expr&)> isProvenNumeric_;
    std::function<bool(const ast::IndexAccess&)> isTypedElem_;
};

bool analyzeForInductionVar(const ast::ForStmt& f, const std::string& name,
                            std::string* outBoundedArrayName = nullptr,
                            std::function<bool(const ast::Expr&)> isProvenNumeric = nullptr,
                            std::function<bool(const ast::IndexAccess&)> isTypedElem = nullptr) {
    if (f.init.size() != 1 || f.condition == nullptr || f.update == nullptr) return false;
    const auto* vd = dynamic_cast<const ast::VarDecl*>(f.init[0].get());
    if (vd == nullptr || vd->isVar || vd->isConst || vd->pattern != nullptr ||
        vd->name != name || vd->init == nullptr) {
        return false;
    }
    int64_t initVal = 0;
    if (!isSafeIntegerLiteral(*vd->init, &initVal)) return false;

    int64_t stepVal = 0;
    if (const auto* u = dynamic_cast<const ast::Unary*>(f.update.get())) {
        const auto* id = dynamic_cast<const ast::Ident*>(u->operand.get());
        if (id == nullptr || id->name != name) return false;
        if (u->op == ast::UnaryOp::PreInc || u->op == ast::UnaryOp::PostInc) {
            stepVal = 1;
        } else if (u->op == ast::UnaryOp::PreDec || u->op == ast::UnaryOp::PostDec) {
            stepVal = -1;
        } else {
            return false;
        }
    } else if (const auto* b = dynamic_cast<const ast::Binary*>(f.update.get())) {
        const auto* id = dynamic_cast<const ast::Ident*>(b->lhs.get());
        if (id == nullptr || id->name != name) return false;
        if (b->op == ast::BinaryOp::PlusAssign && isSafeIntegerLiteral(*b->rhs, &stepVal)) {
        } else if (b->op == ast::BinaryOp::MinusAssign &&
                   isSafeIntegerLiteral(*b->rhs, &stepVal)) {
            stepVal = -stepVal;
        } else {
            return false;
        }
    } else {
        return false;
    }

    const auto* cond = dynamic_cast<const ast::Binary*>(f.condition.get());
    if (cond == nullptr) return false;
    const auto* lhsId = dynamic_cast<const ast::Ident*>(cond->lhs.get());
    const auto* rhsId = dynamic_cast<const ast::Ident*>(cond->rhs.get());
    const bool lhsMatches = (lhsId != nullptr && lhsId->name == name);
    const bool rhsMatches = (rhsId != nullptr && rhsId->name == name);
    if (!lhsMatches && !rhsMatches) return false;
    if (cond->op != ast::BinaryOp::Less && cond->op != ast::BinaryOp::LessEqual &&
        cond->op != ast::BinaryOp::Greater && cond->op != ast::BinaryOp::GreaterEqual) {
        return false;
    }

    std::string candidateArr;
    if (lhsMatches && cond->op == ast::BinaryOp::Less && initVal >= 0 && stepVal == 1) {
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(cond->rhs.get())) {
            if (!mem->optional && mem->property == "length") {
                if (const auto* arrId = dynamic_cast<const ast::Ident*>(mem->object.get())) {
                    candidateArr = arrId->name;
                }
            }
        }
    }

    LoopBodySafetyScan scan(name, candidateArr, std::move(isProvenNumeric),
                            std::move(isTypedElem));
    for (const auto& s : f.body) s->accept(scan);
    if (scan.ivMutatedOrShadowed) return false;
    if (outBoundedArrayName != nullptr && !candidateArr.empty() &&
        !scan.arrMutatedOrShadowed && !scan.hasBufferHazard) {
        *outBoundedArrayName = candidateArr;
    }
    return true;
}

il::ValueId unwrapBoxValue(il::ValueId val, const il::Function& fn,
                           const std::vector<DefSite>& defs) {
    if (val >= defs.size() || defs[val].block == il::kNoBlock) return val;
    const auto& blk = fn.blocks[defs[val].block];
    if (defs[val].index >= blk.instructions.size()) return val;
    const auto& inst = blk.instructions[defs[val].index];
    if (inst.op == il::Op::Box && !inst.operands.empty()) {
        return inst.operands[0];
    }
    return val;
}

bool sameReceiverValue(il::ValueId a, il::ValueId b, const il::Function& fn,
                       const std::vector<DefSite>& defs) {
    return unwrapBoxValue(a, fn, defs) == unwrapBoxValue(b, fn, defs);
}

bool isKnownTypedReceiver(il::ValueId a, il::ValueId recv, const il::Function& fn,
                          const std::vector<DefSite>& defs) {
    if (sameReceiverValue(a, recv, fn, defs)) return true;
    const il::ValueId root = unwrapBoxValue(a, fn, defs);
    for (const il::Block& blk : fn.blocks) {
        for (const il::Instruction& inst : blk.instructions) {
            if ((inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) &&
                !inst.operands.empty() && unwrapBoxValue(inst.operands[0], fn, defs) == root) {
                return true;
            }
        }
    }
    return false;
}

bool isProvablyPrimitiveOrLengthRead(il::ValueId val, il::ValueId recv,
                                     uint32_t lengthKeyIndex, const il::Function& fn,
                                     const std::vector<DefSite>& defs,
                                     const std::vector<uint8_t>& isAccessorKey,
                                     bool protoPatchFree, int depth = 4) {
    if (depth <= 0 || val >= defs.size()) return false;
    if (defs[val].block == il::kNoBlock) {
        return val < fn.params.size() &&
               (fn.params[val].type == il::Type::F64 || fn.params[val].type == il::Type::I32 ||
                fn.params[val].type == il::Type::Bool || fn.params[val].type == il::Type::Str);
    }
    const auto& blk = fn.blocks[defs[val].block];
    if (defs[val].index >= blk.instructions.size()) {
        for (size_t pIdx = 0; pIdx < blk.params.size(); ++pIdx) {
            const auto& bp = blk.params[pIdx];
            if (bp.id != val) continue;
            if (bp.type == il::Type::F64 || bp.type == il::Type::I32 ||
                bp.type == il::Type::Bool || bp.type == il::Type::Str) {
                return true;
            }
            if (depth <= 1) return false;
            bool foundEdge = false;
            for (const auto& predBlk : fn.blocks) {
                if (predBlk.instructions.empty()) continue;
                const auto& term = predBlk.instructions.back();
                auto checkTarget = [&](const il::BlockTarget& t) {
                    if (t.block != defs[val].block) return true;
                    if (pIdx >= t.args.size()) return false;
                    foundEdge = true;
                    return isProvablyPrimitiveOrLengthRead(t.args[pIdx], recv, lengthKeyIndex, fn,
                                                           defs, isAccessorKey, protoPatchFree,
                                                           depth - 1);
                };
                if ((term.op == il::Op::Jump || term.op == il::Op::Branch) &&
                    !checkTarget(term.target)) {
                    return false;
                }
                if (term.op == il::Op::Branch && !checkTarget(term.elseTarget)) {
                    return false;
                }
            }
            return foundEdge;
        }
        return false;
    }

    const auto& inst = blk.instructions[defs[val].index];
    if (inst.type == il::Type::F64 || inst.type == il::Type::I32 ||
        inst.type == il::Type::Bool || inst.type == il::Type::Str) {
        return true;
    }

    switch (inst.op) {
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        case il::Op::ConstBigInt:
        case il::Op::ElemGetTyped:
        case il::Op::TypeOf:
        case il::Op::ToStr:
        case il::Op::ToInt32:
        case il::Op::ToNumeric:
        case il::Op::NumericStep:
        case il::Op::CmpLt:
        case il::Op::CmpGt:
        case il::Op::CmpLe:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::StrictEq:
        case il::Op::LooseEq:
        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::IsNullish:
        case il::Op::IsNumber:
        case il::Op::IsDenseArray:
        case il::Op::NumTruthy:
        case il::Op::MathUnary:
        case il::Op::MathImul:
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::Neg:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::BitNot:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::ConcatEnd:
            return true;

        case il::Op::Box:
            if (inst.boxType == il::Type::F64 || inst.boxType == il::Type::I32 ||
                inst.boxType == il::Type::Bool || inst.boxType == il::Type::Str) {
                return true;
            }
            if (!inst.operands.empty()) {
                return isProvablyPrimitiveOrLengthRead(inst.operands[0], recv, lengthKeyIndex, fn,
                                                       defs, isAccessorKey, protoPatchFree,
                                                       depth - 1);
            }
            return false;

        case il::Op::PropGet:
            if (!protoPatchFree || inst.keyIndex != lengthKeyIndex) return false;
            if (lengthKeyIndex < isAccessorKey.size() && isAccessorKey[lengthKeyIndex]) {
                return false;
            }
            return recv != il::kNoValue && !inst.operands.empty() &&
                   isKnownTypedReceiver(inst.operands[0], recv, fn, defs);

        default:
            return false;
    }
}

bool isSafeInstForInBoundsLoop(const il::Instruction& inst, il::ValueId recv,
                               const il::Function& fn, const std::vector<DefSite>& defs,
                               uint32_t lengthKeyIndex,
                               const std::vector<uint8_t>& isAccessorKey,
                               bool protoPatchFree) {
    switch (inst.op) {
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        case il::Op::ConstBigInt:
        case il::Op::CmpLt:
        case il::Op::CmpGt:
        case il::Op::CmpLe:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::NumTruthy:
        case il::Op::StrictEq:
        case il::Op::IsNullish:
        case il::Op::IsNumber:
        case il::Op::IsDenseArray:
        case il::Op::TypeOf:
        case il::Op::Box:
        case il::Op::CensusRecord:
        case il::Op::KeepAlive:
        case il::Op::PinGuard:
        case il::Op::EnvGet:
        case il::Op::EnvGetTdz:
        case il::Op::ModuleEnvGet:
        case il::Op::ElemGetTyped:
        case il::Op::ElemSetTyped:
        case il::Op::Jump:
        case il::Op::Branch:
        case il::Op::Ret:
            return true;

        case il::Op::MathUnary:
        case il::Op::MathImul:
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Neg:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::BitNot:
            return inst.type == il::Type::F64 || inst.type == il::Type::I32;

        case il::Op::ToInt32:
            return inst.boxType == il::Type::F64 || inst.boxType == il::Type::I32 ||
                inst.boxType == il::Type::Bool;

        case il::Op::PropGet:
            if (!protoPatchFree || inst.keyIndex != lengthKeyIndex) return false;
            if (lengthKeyIndex < isAccessorKey.size() && isAccessorKey[lengthKeyIndex]) {
                return false;
            }
            return !inst.operands.empty() &&
                   isKnownTypedReceiver(inst.operands[0], recv, fn, defs);

        case il::Op::Unbox:
            if (inst.rawUnbox || inst.nullishUnbox) return true;
            if (inst.operands.empty()) return false;
            return isProvablyPrimitiveOrLengthRead(inst.operands[0], recv, lengthKeyIndex, fn,
                                                   defs, isAccessorKey, protoPatchFree);

        default:
            return false;
    }
}

const ast::VarDecl* findUniqueConstDecl(
    const std::string& name, const ast::IndexAccess* site,
    const std::vector<std::unique_ptr<ast::Stmt>>* bodyStmts) {
    if (bodyStmts == nullptr) return nullptr;
    if (site != nullptr) {
        EnclosingForFinder forFinder(*site);
        for (const auto& s : *bodyStmts) s->accept(forFinder);
        for (auto it = forFinder.enclosingBlocks.rbegin(); it != forFinder.enclosingBlocks.rend();
             ++it) {
            const ast::BlockStmt* blk = *it;
            ConstDeclFinder finder(name);
            for (const auto& s : blk->stmts) s->accept(finder);
            if (finder.count == 1 && finder.decl != nullptr && finder.decl->isConst &&
                finder.decl->init != nullptr) {
                for (const auto& s : blk->stmts) {
                    if (s.get() == finder.decl) return finder.decl;
                }
            }
        }
    }
    ConstDeclFinder finder(name);
    for (const auto& s : *bodyStmts) s->accept(finder);
    if (finder.count == 1 && finder.decl != nullptr && finder.decl->isConst &&
        finder.decl->init != nullptr) {
        return finder.decl;
    }
    return nullptr;
}

bool resolveSafeIntegerOrConst(const ast::Expr& e,
                               const std::vector<std::unique_ptr<ast::Stmt>>* bodyStmts,
                               const std::unordered_set<std::string>& memoryNames,
                               int64_t* outVal = nullptr,
                               const ast::IndexAccess* site = nullptr) {
    if (isSafeIntegerLiteral(e, outVal)) return true;
    const auto* id = dynamic_cast<const ast::Ident*>(&e);
    if (id == nullptr || bodyStmts == nullptr || memoryNames.count(id->name) != 0) return false;
    if (const ast::VarDecl* decl = findUniqueConstDecl(id->name, site, bodyStmts)) {
        return isSafeIntegerLiteral(*decl->init, outVal);
    }
    return false;
}

}  // namespace

bool Lowerer::provenIntegralIndexExpr(const ast::Expr& e, const ast::IndexAccess& site,
                                      int depth) const {
    if (depth <= 0) return false;
    if (isSafeIntegerLiteral(e)) return true;
    if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) {
        return u->op == ast::UnaryOp::BitNot;
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(&e)) {
        switch (b->op) {
            case ast::BinaryOp::BitAnd:
            case ast::BinaryOp::BitOr:
            case ast::BinaryOp::BitXor:
            case ast::BinaryOp::Shl:
            case ast::BinaryOp::Shr:
            case ast::BinaryOp::UShr:
                return true;
            case ast::BinaryOp::Add:
            case ast::BinaryOp::Sub:
                return provenIntegralIndexExpr(*b->lhs, site, depth - 1) &&
                       provenIntegralIndexExpr(*b->rhs, site, depth - 1);
            case ast::BinaryOp::Mul: {
                int64_t lit = 0;
                if (resolveSafeIntegerOrConst(*b->rhs, currentBodyStmts_, memoryNames_, &lit,
                                              &site) &&
                    std::abs(lit) <= 65536) {
                    return provenIntegralIndexExpr(*b->lhs, site, 1);
                }
                if (resolveSafeIntegerOrConst(*b->lhs, currentBodyStmts_, memoryNames_, &lit,
                                              &site) &&
                    std::abs(lit) <= 65536) {
                    return provenIntegralIndexExpr(*b->rhs, site, 1);
                }
                return false;
            }
            default:
                return false;
        }
    }
    if (const auto* call = dynamic_cast<const ast::Call*>(&e)) {
        if (pristineMathCall(e)) {
            if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
                if (mem->property == "imul" || mem->property == "clz32") return true;
            }
        }
        return false;
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
        if (currentBodyStmts_ == nullptr || memoryNames_.count(id->name) != 0) return false;
        EnclosingForFinder forFinder(site);
        for (const auto& s : *currentBodyStmts_) s->accept(forFinder);
        for (const ast::ForStmt* f : forFinder.enclosing) {
            if (analyzeForInductionVar(*f, id->name)) return true;
        }
        if (const ast::VarDecl* decl = findUniqueConstDecl(id->name, &site, currentBodyStmts_)) {
            return provenIntegralIndexExpr(*decl->init, site, depth - 1);
        }
    }
    return false;
}

uint32_t Lowerer::typedElemProofFlags(const ast::IndexAccess& idx, uint32_t elemKind) const {
    if (typedElemDisabled_ || currentBodyStmts_ == nullptr) return 0;
    uint32_t flags = 0;
    if (provenIntegralIndexExpr(*idx.index, idx, 4)) {
        flags |= static_cast<uint32_t>(il::kElemFlagProvenIntegralIndex);
    }
    const uint32_t rawKind = elemKind & static_cast<uint32_t>(il::kElemKindMask);
    if (rawKind <= 11 && (flags & static_cast<uint32_t>(il::kElemFlagProvenIntegralIndex)) != 0) {
        const auto* recvId = dynamic_cast<const ast::Ident*>(idx.object.get());
        const auto* idxId = dynamic_cast<const ast::Ident*>(idx.index.get());
        if (recvId != nullptr && memoryNames_.count(recvId->name) == 0) {
            if (const auto* bin = dynamic_cast<const ast::Binary*>(idx.index.get());
                bin != nullptr && bin->op == ast::BinaryOp::BitAnd) {
                int64_t mask = -1;
                if (!resolveSafeIntegerOrConst(*bin->rhs, currentBodyStmts_, memoryNames_, &mask,
                                               &idx) ||
                    mask < 0) {
                    mask = -1;
                    resolveSafeIntegerOrConst(*bin->lhs, currentBodyStmts_, memoryNames_, &mask,
                                              &idx);
                }
                if (mask >= 0) {
                    if (const ast::VarDecl* decl =
                            findUniqueConstDecl(recvId->name, &idx, currentBodyStmts_)) {
                        if (const auto* ne = dynamic_cast<const ast::NewExpr*>(decl->init.get());
                            ne != nullptr && ne->args.size() == 1) {
                            int64_t len = 0;
                            if (resolveSafeIntegerOrConst(*ne->args[0], currentBodyStmts_,
                                                          memoryNames_, &len, &idx) &&
                                mask < len) {
                                flags |= static_cast<uint32_t>(il::kElemFlagProvenInBounds);
                            }
                        }
                    }
                }
            }
        }
        if (recvId != nullptr && idxId != nullptr &&
            memoryNames_.count(recvId->name) == 0 &&
            memoryNames_.count(idxId->name) == 0) {
            auto isNumeric = [this](const ast::Expr& expr) {
                return provenNumber(expr) || definitelyNumericOperand(expr, 6);
            };
            auto isTypedElem = [this](const ast::IndexAccess& ia) {
                return typedElemAccessKind(ia, true).has_value();
            };
            EnclosingForFinder forFinder(idx);
            for (const auto& s : *currentBodyStmts_) s->accept(forFinder);
            for (const ast::ForStmt* f : forFinder.enclosing) {
                std::string boundedArr;
                if (analyzeForInductionVar(*f, idxId->name, &boundedArr, isNumeric, isTypedElem) &&
                    boundedArr == recvId->name) {
                    flags |= static_cast<uint32_t>(il::kElemFlagProvenInBounds);
                    break;
                }
            }
        }
    }
    return flags;
}

bool canMutateLengthViaCoercion(const il::Instruction& inst, il::ValueId recv,
                                uint32_t lengthKeyIndex, const il::Function& fn,
                                const std::vector<DefSite>& defs,
                                const std::vector<uint8_t>& isAccessorKey,
                                bool protoPatchFree) {
    auto opIsSafe = [&](il::ValueId v) {
        return isProvablyPrimitiveOrLengthRead(v, recv, lengthKeyIndex, fn, defs, isAccessorKey,
                                               protoPatchFree);
    };
    switch (inst.op) {
        case il::Op::Unbox:
            if (inst.rawUnbox || inst.nullishUnbox) return false;
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::ToInt32:
            if (inst.boxType == il::Type::F64 || inst.boxType == il::Type::I32 ||
                inst.boxType == il::Type::Bool) {
                return false;
            }
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::ToNumeric:
        case il::Op::NumericStep:
        case il::Op::ToStr:
        case il::Op::ConcatAppend:
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::Neg:
        case il::Op::BitNot:
            if (inst.type != il::Type::Dynamic) return false;
            return inst.operands.empty() || !opIsSafe(inst.operands[0]);

        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
            if (inst.type != il::Type::Dynamic) return false;
            return inst.operands.size() < 2 || !opIsSafe(inst.operands[0]) ||
                   !opIsSafe(inst.operands[1]);

        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::LooseEq:
            return inst.operands.size() < 2 || !opIsSafe(inst.operands[0]) ||
                   !opIsSafe(inst.operands[1]);

        case il::Op::ElemGet:
        case il::Op::In:
        case il::Op::InstanceOf:
        case il::Op::ObjectKeys:
        case il::Op::ForInKeys:
            return true;

        default:
            return false;
    }
}

void sanitizeLoopInBoundsProofs(il::Module& module) {
    uint32_t lengthKeyIndex = UINT32_MAX;
    std::vector<uint8_t> isAccessorKey(module.keyConstants.size(), 0);
    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "length") lengthKeyIndex = static_cast<uint32_t>(k);
        if (name == "size" || name == "flags" || name == "source" || name == "buffer" ||
            name == "byteLength" || name == "byteOffset" || name == "__proto__" ||
            name == "lastIndex") {
            isAccessorKey[k] = 1;
        }
    }
    for (const il::Function& fn : module.functions) {
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if (inst.op == il::Op::AccessorDef && inst.keyIndex < isAccessorKey.size()) {
                    isAccessorKey[inst.keyIndex] = 1;
                }
            }
        }
    }
    const bool protoPatchFree = isModulePrototypePatchFree(module);

    struct LoopDesc {
        il::BlockId header;
        std::vector<uint8_t> inLoop;
        std::vector<il::BlockId> loopBlocks;
    };

    for (il::Function& fn : module.functions) {
        bool hasProvenInBounds = false;
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if ((inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) &&
                    (inst.immI32 & static_cast<int32_t>(il::kElemFlagProvenInBounds)) != 0) {
                    hasProvenInBounds = true;
                    break;
                }
            }
            if (hasProvenInBounds) break;
        }
        if (!hasProvenInBounds) continue;

        const Cfg cfg = buildCfg(fn);
        const std::vector<DefSite> defs = computeDefSites(fn);
        std::vector<LoopDesc> loops;
        for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
            std::vector<il::BlockId> backedges;
            for (il::BlockId pred : cfg.preds[header]) {
                if (cfg.dominates(header, pred)) backedges.push_back(pred);
            }
            if (backedges.empty()) continue;
            LoopDesc ld{header, std::vector<uint8_t>(fn.blocks.size(), 0), {}};
            for (il::BlockId tail : backedges) {
                for (il::BlockId lb : naturalLoop(cfg, header, tail)) {
                    if (lb < ld.inLoop.size() && !ld.inLoop[lb]) {
                        ld.inLoop[lb] = 1;
                        ld.loopBlocks.push_back(lb);
                    }
                }
            }
            loops.push_back(std::move(ld));
        }

        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            for (il::Instruction& inst : fn.blocks[b].instructions) {
                if ((inst.op != il::Op::ElemGetTyped && inst.op != il::Op::ElemSetTyped) ||
                    (inst.immI32 & static_cast<int32_t>(il::kElemFlagProvenInBounds)) == 0) {
                    continue;
                }
                bool valid = false;
                if (inst.operands.size() >= 2) {
                    const il::ValueId recv = inst.operands[0];
                    il::ValueId idxRoot = inst.operands[1];
                    if (idxRoot < defs.size() && defs[idxRoot].block != il::kNoBlock) {
                        const auto& dBlk = fn.blocks[defs[idxRoot].block];
                        if (defs[idxRoot].index < dBlk.instructions.size()) {
                            const auto& di = dBlk.instructions[defs[idxRoot].index];
                            if (di.op == il::Op::Unbox && !di.operands.empty()) {
                                idxRoot = di.operands[0];
                            }
                        }
                    }

                    for (const LoopDesc& ld : loops) {
                        if (b >= ld.inLoop.size() || !ld.inLoop[b]) continue;
                        bool isBoundedIdx = false;
                        for (const auto& bp : fn.blocks[ld.header].params) {
                            if (bp.id == idxRoot) {
                                isBoundedIdx = true;
                                break;
                            }
                        }
                        if (!isBoundedIdx && idxRoot < defs.size() &&
                            defs[idxRoot].block != il::kNoBlock) {
                            const auto& iBlk = fn.blocks[defs[idxRoot].block];
                            if (defs[idxRoot].index < iBlk.instructions.size() &&
                                iBlk.instructions[defs[idxRoot].index].op == il::Op::BitAnd) {
                                isBoundedIdx = true;
                            }
                        }
                        if (!isBoundedIdx) continue;

                        const il::ValueId recvRoot = unwrapBoxValue(recv, fn, defs);
                        if (recvRoot < defs.size() && defs[recvRoot].block != il::kNoBlock &&
                            defs[recvRoot].block < ld.inLoop.size() &&
                            ld.inLoop[defs[recvRoot].block]) {
                            continue;
                        }

                        bool loopSafe = true;
                        for (il::BlockId lb : ld.loopBlocks) {
                            if (fn.blocks[lb].handler != il::kNoBlock) {
                                loopSafe = false;
                                break;
                            }
                            for (const il::Instruction& loopInst : fn.blocks[lb].instructions) {
                                if (!isSafeInstForInBoundsLoop(loopInst, recv, fn, defs,
                                                               lengthKeyIndex, isAccessorKey,
                                                               protoPatchFree)) {
                                    loopSafe = false;
                                    break;
                                }
                            }
                            if (!loopSafe) break;
                        }
                        if (loopSafe) {
                            valid = true;
                            break;
                        }
                    }
                }
                if (!valid) {
                    inst.immI32 &= ~static_cast<int32_t>(il::kElemFlagProvenInBounds);
                }
            }
        }
    }
}

}  // namespace bronze::lower
