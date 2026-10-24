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
    }
    void visit(const ast::FunctionExpr&) override {}
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
            (n->value == 0.0 && std::signbit(n->value)) ||
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
                if (inner == 0 || inner == -2147483648LL) return false;
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
    explicit EnclosingForFinder(const ast::Node& target) : target_(&target) {}

    std::vector<const ast::ForStmt*> enclosing;
    std::vector<const std::vector<ast::StmtPtr>*> enclosingStmtLists;

    void checkTarget(const ast::Node& n) {
        if (&n == target_) {
            enclosing = stack_;
            enclosingStmtLists = stmtLists_;
        }
    }
    void visit(const ast::IndexAccess& ia) override {
        checkTarget(ia);
        ast::detail::IdentVisitor::visit(ia);
    }
    void visit(const ast::Binary& b) override {
        checkTarget(b);
        ast::detail::IdentVisitor::visit(b);
    }
    void visit(const ast::Ident& i) override {
        checkTarget(i);
        ast::detail::IdentVisitor::visit(i);
    }
    void visit(const ast::VarDecl& v) override {
        checkTarget(v);
        ast::detail::IdentVisitor::visit(v);
    }
    void visit(const ast::BlockStmt& b) override {
        checkTarget(b);
        stmtLists_.push_back(&b.stmts);
        for (const auto& s : b.stmts) s->accept(*this);
        stmtLists_.pop_back();
    }
    void visit(const ast::IfStmt& i) override {
        checkTarget(i);
        if (i.condition) i.condition->accept(*this);
        stmtLists_.push_back(&i.thenBody);
        for (const auto& s : i.thenBody) s->accept(*this);
        stmtLists_.pop_back();
        if (!i.elseBody.empty()) {
            stmtLists_.push_back(&i.elseBody);
            for (const auto& s : i.elseBody) s->accept(*this);
            stmtLists_.pop_back();
        }
    }
    void visit(const ast::WhileStmt& w) override {
        checkTarget(w);
        if (w.condition) w.condition->accept(*this);
        stmtLists_.push_back(&w.body);
        for (const auto& s : w.body) s->accept(*this);
        stmtLists_.pop_back();
    }
    void visit(const ast::DoWhileStmt& d) override {
        checkTarget(d);
        stmtLists_.push_back(&d.body);
        for (const auto& s : d.body) s->accept(*this);
        stmtLists_.pop_back();
        if (d.condition) d.condition->accept(*this);
    }
    void visit(const ast::ForStmt& f) override {
        checkTarget(f);
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        stack_.push_back(&f);
        stmtLists_.push_back(&f.body);
        for (const auto& s : f.body) s->accept(*this);
        stmtLists_.pop_back();
        stack_.pop_back();
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::FunctionExpr&) override {}

private:
    const ast::Node* target_;
    std::vector<const ast::ForStmt*> stack_;
    std::vector<const std::vector<ast::StmtPtr>*> stmtLists_;
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

const ast::VarDecl* findUniqueConstDecl(
    const std::string& name, const ast::Node* site,
    const std::vector<std::unique_ptr<ast::Stmt>>* bodyStmts) {
    if (bodyStmts == nullptr) return nullptr;
    if (site != nullptr) {
        EnclosingForFinder forFinder(*site);
        for (const auto& s : *bodyStmts) s->accept(forFinder);
        for (auto it = forFinder.enclosingStmtLists.rbegin();
             it != forFinder.enclosingStmtLists.rend(); ++it) {
            const auto* stmts = *it;
            int directCount = 0;
            const ast::VarDecl* directDecl = nullptr;
            for (const auto& s : *stmts) {
                if (const auto* vd = dynamic_cast<const ast::VarDecl*>(s.get())) {
                    if (vd->pattern) {
                        for (const auto& n : ast::patternBoundNames(*vd->pattern)) {
                            if (n == name) ++directCount;
                        }
                    } else if (vd->name == name) {
                        ++directCount;
                        directDecl = vd;
                    }
                } else if (const auto* fd = dynamic_cast<const ast::FunctionDecl*>(s.get())) {
                    if (fd->name == name) ++directCount;
                }
            }
            if (directCount > 0) {
                return (directCount == 1 && directDecl != nullptr && directDecl->isConst &&
                        !directDecl->isVar && directDecl->init != nullptr)
                           ? directDecl
                           : nullptr;
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
                               const FnParamFacts* paramFacts,
                               int64_t* outVal = nullptr,
                               const ast::Node* site = nullptr) {
    if (isSafeIntegerLiteral(e, outVal)) return true;
    const auto* id = dynamic_cast<const ast::Ident*>(&e);
    if (id == nullptr || bodyStmts == nullptr || memoryNames.count(id->name) != 0) return false;
    if (const ast::VarDecl* decl = findUniqueConstDecl(id->name, site, bodyStmts)) {
        return isSafeIntegerLiteral(*decl->init, outVal);
    }
    if (paramFacts != nullptr) {
        auto it = paramFacts->constIntParams.find(id->name);
        if (it != paramFacts->constIntParams.end()) {
            if (outVal) *outVal = it->second;
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<std::pair<int64_t, int64_t>> Lowerer::evalIndexInterval(
    const ast::Expr& e, const ast::Node& site, int depth,
    std::string* outBoundedArrayName) const {
    if (depth <= 0 || currentBodyStmts_ == nullptr) return std::nullopt;
    int64_t lit = 0;
    if (isSafeIntegerLiteral(e, &lit)) return std::make_pair(lit, lit);

    if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) {
        if (!u->operand) return std::nullopt;
        if (u->op == ast::UnaryOp::Posate) {
            return evalIndexInterval(*u->operand, site, depth - 1);
        }
        if (u->op == ast::UnaryOp::Negate) {
            auto inner = evalIndexInterval(*u->operand, site, depth - 1);
            if (!inner || (inner->first <= 0 && inner->second >= 0) ||
                inner->first <= -2147483648LL) {
                return std::nullopt;
            }
            return std::make_pair(-inner->second, -inner->first);
        }
        if (u->op == ast::UnaryOp::BitNot) {
            return std::make_pair(-2147483648LL, 2147483647LL);
        }
        return std::nullopt;
    }

    if (const auto* b = dynamic_cast<const ast::Binary*>(&e)) {
        if (!b->lhs || !b->rhs) return std::nullopt;
        switch (b->op) {
            case ast::BinaryOp::Add: {
                auto l = evalIndexInterval(*b->lhs, site, depth - 1);
                auto r = evalIndexInterval(*b->rhs, site, depth - 1);
                if (!l || !r) return std::nullopt;
                const int64_t lo = l->first + r->first;
                const int64_t hi = l->second + r->second;
                if (lo < -2147483648LL || hi > 2147483647LL || lo > hi) return std::nullopt;
                return std::make_pair(lo, hi);
            }
            case ast::BinaryOp::Sub: {
                auto l = evalIndexInterval(*b->lhs, site, depth - 1);
                auto r = evalIndexInterval(*b->rhs, site, depth - 1);
                if (!l || !r) return std::nullopt;
                const int64_t lo = l->first - r->second;
                const int64_t hi = l->second - r->first;
                if (lo < -2147483648LL || hi > 2147483647LL || lo > hi) return std::nullopt;
                return std::make_pair(lo, hi);
            }
            case ast::BinaryOp::Mul: {
                auto l = evalIndexInterval(*b->lhs, site, depth - 1);
                auto r = evalIndexInterval(*b->rhs, site, depth - 1);
                if (!l || !r) return std::nullopt;
                if ((l->first < 0 && r->first <= 0 && r->second >= 0) ||
                    (r->first < 0 && l->first <= 0 && l->second >= 0)) {
                    return std::nullopt;
                }
                const int64_t p1 = l->first * r->first;
                const int64_t p2 = l->first * r->second;
                const int64_t p3 = l->second * r->first;
                const int64_t p4 = l->second * r->second;
                const int64_t lo = std::min({p1, p2, p3, p4});
                const int64_t hi = std::max({p1, p2, p3, p4});
                if (lo < -2147483648LL || hi > 2147483647LL || lo > hi) return std::nullopt;
                return std::make_pair(lo, hi);
            }
            case ast::BinaryOp::BitAnd: {
                auto r = evalIndexInterval(*b->rhs, site, depth - 1);
                if (r && r->first == r->second && r->first >= 0) {
                    return std::make_pair(int64_t{0}, r->first);
                }
                auto l = evalIndexInterval(*b->lhs, site, depth - 1);
                if (l && l->first == l->second && l->first >= 0) {
                    return std::make_pair(int64_t{0}, l->first);
                }
                if (l && r && l->first >= 0 && r->first >= 0) {
                    return std::make_pair(int64_t{0}, std::min(l->second, r->second));
                }
                return std::make_pair(-2147483648LL, 2147483647LL);
            }
            case ast::BinaryOp::UShr: {
                auto r = evalIndexInterval(*b->rhs, site, depth - 1);
                if (r && r->first == r->second && (r->first & 31) > 0) {
                    return std::make_pair(
                        int64_t{0}, static_cast<int64_t>(0xFFFFFFFFu >> (r->first & 31)));
                }
                return std::nullopt;
            }
            case ast::BinaryOp::BitOr:
            case ast::BinaryOp::BitXor:
            case ast::BinaryOp::Shl:
            case ast::BinaryOp::Shr:
                return std::make_pair(-2147483648LL, 2147483647LL);
            default:
                return std::nullopt;
        }
    }

    if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
        if (memoryNames_.count(id->name) != 0) return std::nullopt;
        if (const ast::VarDecl* decl = findUniqueConstDecl(id->name, &site, currentBodyStmts_)) {
            return evalIndexInterval(*decl->init, *decl, depth - 1);
        }
        EnclosingForFinder forFinder(site);
        for (const auto& s : *currentBodyStmts_) s->accept(forFinder);
        for (const ast::ForStmt* f : forFinder.enclosing) {
            if (f->init.size() != 1 || !f->condition || !f->update) continue;
            const auto* vd = dynamic_cast<const ast::VarDecl*>(f->init[0].get());
            if (!vd || vd->isVar || vd->isConst || vd->pattern || vd->name != id->name ||
                !vd->init) {
                continue;
            }
            std::string boundedArr;
            auto isNumeric = [this](const ast::Expr& expr) {
                return provenNumber(expr) || definitelyNumericOperand(expr, 6);
            };
            auto isTypedElem = [this](const ast::IndexAccess& ia) {
                return typedElemAccessKind(ia, true).has_value();
            };
            if (!analyzeForInductionVar(*f, id->name, &boundedArr, isNumeric, isTypedElem)) {
                continue;
            }
            int64_t initVal = 0;
            if (!isSafeIntegerLiteral(*vd->init, &initVal)) continue;
            int64_t stepVal = 1;
            if (const auto* u = dynamic_cast<const ast::Unary*>(f->update.get())) {
                stepVal = (u->op == ast::UnaryOp::PreDec || u->op == ast::UnaryOp::PostDec) ? -1 : 1;
            } else if (const auto* ub = dynamic_cast<const ast::Binary*>(f->update.get())) {
                isSafeIntegerLiteral(*ub->rhs, &stepVal);
                if (ub->op == ast::BinaryOp::MinusAssign) stepVal = -stepVal;
            }
            const auto* cond = dynamic_cast<const ast::Binary*>(f->condition.get());
            if (!cond) continue;
            const auto* lhsId = dynamic_cast<const ast::Ident*>(cond->lhs.get());
            if (stepVal > 0 && lhsId && lhsId->name == id->name &&
                (cond->op == ast::BinaryOp::Less || cond->op == ast::BinaryOp::LessEqual)) {
                if (!boundedArr.empty()) {
                    if (outBoundedArrayName) *outBoundedArrayName = boundedArr;
                    int64_t hi = 2147483646LL;
                    if (currentFnParamFacts_ != nullptr) {
                        auto mIt = currentFnParamFacts_->maxTypedArrayLenParams.find(boundedArr);
                        if (mIt != currentFnParamFacts_->maxTypedArrayLenParams.end() &&
                            mIt->second > initVal) {
                            hi = mIt->second - 1;
                        }
                    }
                    return std::make_pair(initVal, std::max(initVal, hi));
                }
                auto bnd = evalIndexInterval(*cond->rhs, *f, depth - 1);
                if (bnd) {
                    const int64_t hi =
                        (cond->op == ast::BinaryOp::Less) ? (bnd->second - 1) : bnd->second;
                    if (initVal <= hi && initVal >= -2147483648LL && hi <= 2147483647LL) {
                        return std::make_pair(initVal, hi);
                    }
                }
            }
        }
        if (currentFnParamFacts_ != nullptr) {
            auto it = currentFnParamFacts_->intParamBounds.find(id->name);
            if (it != currentFnParamFacts_->intParamBounds.end()) {
                return it->second;
            }
        }
    }
    return std::nullopt;
}

bool Lowerer::provenIntegralIndexExpr(const ast::Expr& e, const ast::IndexAccess& site,
                                      int depth) const {
    if (depth <= 0) return false;
    if (evalIndexInterval(e, site, depth).has_value()) return true;
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
                if (resolveSafeIntegerOrConst(*b->rhs, currentBodyStmts_, memoryNames_,
                                              currentFnParamFacts_, &lit, &site) &&
                    std::abs(lit) <= 65536) {
                    return provenIntegralIndexExpr(*b->lhs, site, 1);
                }
                if (resolveSafeIntegerOrConst(*b->lhs, currentBodyStmts_, memoryNames_,
                                              currentFnParamFacts_, &lit, &site) &&
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
    std::string boundedArr;
    const auto idxInterval = evalIndexInterval(*idx.index, idx, 8, &boundedArr);
    if (idxInterval.has_value() || provenIntegralIndexExpr(*idx.index, idx, 6)) {
        flags |= static_cast<uint32_t>(il::kElemFlagProvenIntegralIndex);
    }
    const uint32_t rawKind = elemKind & static_cast<uint32_t>(il::kElemKindMask);
    if (rawKind <= 11 && (flags & static_cast<uint32_t>(il::kElemFlagProvenIntegralIndex)) != 0) {
        const auto* recvId = dynamic_cast<const ast::Ident*>(idx.object.get());
        if (recvId != nullptr && memoryNames_.count(recvId->name) == 0) {
            if (!boundedArr.empty() && boundedArr == recvId->name) {
                flags |= static_cast<uint32_t>(il::kElemFlagProvenInBounds);
            } else if (idxInterval.has_value() && idxInterval->first >= 0) {
                std::optional<int64_t> minLen;
                if (currentFnParamFacts_ != nullptr) {
                    auto it = currentFnParamFacts_->minTypedArrayLenParams.find(recvId->name);
                    if (it != currentFnParamFacts_->minTypedArrayLenParams.end()) {
                        minLen = it->second;
                    }
                }
                if (!minLen && moduleBufferDetachFree_) {
                    if (const ast::VarDecl* decl =
                            findUniqueConstDecl(recvId->name, &idx, currentBodyStmts_)) {
                        if (const auto* ne = dynamic_cast<const ast::NewExpr*>(decl->init.get());
                            ne != nullptr && ne->args.size() == 1) {
                            int64_t len = 0;
                            if (resolveSafeIntegerOrConst(*ne->args[0], currentBodyStmts_,
                                                          memoryNames_, currentFnParamFacts_,
                                                          &len, &idx) &&
                                len > 0) {
                                minLen = len;
                            }
                        }
                    }
                }
                if (minLen.has_value() && idxInterval->second < *minLen) {
                    flags |= static_cast<uint32_t>(il::kElemFlagProvenInBounds);
                }
            }
        }
    }
    return flags;
}

}  // namespace bronze::lower
