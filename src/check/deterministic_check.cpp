#include "check/deterministic_check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lex/lexer.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

bool isTranscendentalMathMethod(std::string_view name) {
    static const std::unordered_set<std::string_view> kMethods = {
        "sin",   "cos",   "tan",   "asin",  "acos",  "atan",  "atan2",
        "sinh",  "cosh",  "tanh",  "asinh", "acosh", "atanh", "exp",
        "expm1", "log",   "log1p", "log10", "log2",  "pow",   "cbrt",
        "hypot",
    };
    return kMethods.count(name) > 0;
}

bool isDeterministicMathMethod(std::string_view name) {
    static const std::unordered_set<std::string_view> kDetMethods = {
        "sin",  "cos",   "tan", "asin", "acos",  "atan",
        "atan2", "exp",  "log", "pow",  "hypot", "cbrt",
    };
    return kDetMethods.count(name) > 0;
}

bool isObjectEnumerationMethod(std::string_view name) {
    return name == "keys" || name == "values" || name == "entries";
}

bool isWhitelistedGlobalObject(std::string_view name) {
    static const std::unordered_set<std::string_view> kSafeGlobals = {
        "Math",          "Object",        "Array",         "Number",
        "String",        "Boolean",       "Symbol",        "BigInt",
        "Int8Array",     "Uint8Array",    "Uint8ClampedArray",
        "Int16Array",    "Uint16Array",   "Int32Array",    "Uint32Array",
        "Float16Array",  "Float32Array",  "Float64Array",  "BigInt64Array",
        "BigUint64Array","ArrayBuffer",   "DataView",      "Map",
        "Set",           "JSON",          "Error",         "TypeError",
        "RangeError",    "SyntaxError",   "ReferenceError","NaN",
        "Infinity",      "undefined",     "isNaN",         "isFinite",
        "parseInt",      "parseFloat",    "Date",          "performance",
        "globalThis",
    };
    return kSafeGlobals.count(name) > 0;
}

bool hasUseDeterministicPrologue(const std::vector<ast::StmtPtr>& body) {
    for (const auto& stmtPtr : body) {
        const auto* exprStmt = ast::exactAs<const ast::ExprStmt*>(stmtPtr.get());
        if (!exprStmt || !exprStmt->expr || exprStmt->expr->parenthesized) break;
        const auto* strLit = ast::exactAs<const ast::StringLit*>(exprStmt->expr.get());
        if (!strLit) break;
        if (strLit->value == "use deterministic") return true;
    }
    return false;
}

bool isIntegerLiteralExpr(const ast::Expr* expr) {
    if (!expr) return false;
    if (auto* num = ast::exactAs<const ast::NumberLit*>(expr)) {
        return std::isfinite(num->value) && std::trunc(num->value) == num->value;
    }
    if (auto* un = ast::exactAs<const ast::Unary*>(expr)) {
        if (un->op == ast::UnaryOp::Negate || un->op == ast::UnaryOp::Posate) {
            return isIntegerLiteralExpr(un->operand.get());
        }
    }
    return false;
}

class DeterministicAstVisitor final : public ast::Visitor {
public:
    explicit DeterministicAstVisitor(std::vector<CheckDiagnostic>& diags, bool moduleDeterministicFp)
        : diags_(diags) {
        fnStack_.push_back("<top-level>");
        detFpStack_.push_back(moduleDeterministicFp);
        scopeStack_.emplace_back();
    }

    void visit(const ast::NumberLit&) override {}
    void visit(const ast::BigIntLit&) override {}
    void visit(const ast::SpreadElement& n) override {
        if (n.argument) n.argument->accept(*this);
    }
    void visit(const ast::StringLit&) override {}
    void visit(const ast::TemplateLit& n) override {
        for (const auto& e : n.exprs) if (e) e->accept(*this);
    }
    void visit(const ast::TaggedTemplate& n) override {
        if (n.tag) n.tag->accept(*this);
        if (n.templateLit) n.templateLit->accept(*this);
    }
    void visit(const ast::RegExpLit&) override {}
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident& n) override {
        checkIdentifierAccess(n.name, n.span, /*isWrite=*/false);
    }
    void visit(const ast::Unary& n) override {
        if (n.op == ast::UnaryOp::PreInc || n.op == ast::UnaryOp::PreDec ||
            n.op == ast::UnaryOp::PostInc || n.op == ast::UnaryOp::PostDec) {
            if (auto* id = ast::exactAs<const ast::Ident*>(n.operand.get())) {
                checkIdentifierAccess(id->name, n.span, /*isWrite=*/true);
                return;
            }
            checkTargetMutation(n.operand.get(), n.span);
        } else if (n.op == ast::UnaryOp::Delete) {
            checkTargetMutation(n.operand.get(), n.span);
        }
        if (n.operand) n.operand->accept(*this);
    }
    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Exp || n.op == ast::BinaryOp::ExpAssign) {
            if (!currentDeterministicFp() &&
                (!isIntegerLiteralExpr(n.lhs.get()) || !isIntegerLiteralExpr(n.rhs.get()))) {
                emit(DeterministicHazardKind::TranscendentalFloatDrift, n.span,
                     "exponentiation operator (**) uses platform pow() which may drift by 1 ULP across x86_64 and AArch64",
                     "add \"use deterministic\"; or use repeated integer multiplication");
            }
        }
        if (ast::isAssignOp(n.op)) {
            if (auto* id = ast::exactAs<const ast::Ident*>(n.lhs.get())) {
                recordAliasFromExpr(id->name, n.rhs.get());
                checkIdentifierAccess(id->name, n.span, /*isWrite=*/true);
                if (n.rhs) n.rhs->accept(*this);
                return;
            }
            checkTargetMutation(n.lhs.get(), n.span);
        }
        if (n.lhs) n.lhs->accept(*this);
        if (n.rhs) n.rhs->accept(*this);
    }
    void visit(const ast::Ternary& n) override {
        if (n.condition) n.condition->accept(*this);
        if (n.thenExpr) n.thenExpr->accept(*this);
        if (n.elseExpr) n.elseExpr->accept(*this);
    }
    void visit(const ast::MemberAccess& n) override {
        if (n.object) n.object->accept(*this);
    }
    void visit(const ast::IndexAccess& n) override {
        if (n.object) n.object->accept(*this);
        if (n.index) n.index->accept(*this);
    }
    void visit(const ast::Call& n) override {
        if (auto* mem = ast::exactAs<const ast::MemberAccess*>(n.callee.get())) {
            if (mem->property == "sort" && mem->object) {
                if (auto* innerCall = ast::exactAs<const ast::Call*>(mem->object.get())) {
                    sortedObjectCalls_.insert(innerCall);
                }
            }
            std::string recvGlobal = resolveGlobalObjectExpr(mem->object.get());
            if (!recvGlobal.empty()) checkGlobalMemberCall(recvGlobal, mem->property, &n, n.span);
        } else if (auto* calleeIdent = ast::exactAs<const ast::Ident*>(n.callee.get())) {
            if (auto itM = aliasToMethod_.find(calleeIdent->name); itM != aliasToMethod_.end()) {
                checkGlobalMemberCall(itM->second.first, itM->second.second, &n, n.span);
            }
        }
        if (n.callee) n.callee->accept(*this);
        for (const auto& arg : n.args) if (arg) arg->accept(*this);
    }
    void visit(const ast::NewExpr& n) override {
        if (resolveGlobalObjectExpr(n.callee.get()) == "Date" && n.args.empty()) {
            emit(DeterministicHazardKind::WallClockTime, n.span,
                 "new Date() with no arguments captures non-deterministic host wall-clock time",
                 "pass an explicit deterministic epoch timestamp or use simulation ticks");
        }
        if (n.callee) n.callee->accept(*this);
        for (const auto& arg : n.args) if (arg) arg->accept(*this);
    }
    void visit(const ast::NewTargetExpr&) override {}
    void visit(const ast::ImportMetaExpr&) override {}
    void visit(const ast::SuperCall& n) override {
        if (n.baseExpr) n.baseExpr->accept(*this);
        for (const auto& arg : n.args) if (arg) arg->accept(*this);
    }
    void visit(const ast::SuperMember& n) override {
        if (n.baseExpr) n.baseExpr->accept(*this);
        if (n.propertyExpr) n.propertyExpr->accept(*this);
    }
    void visit(const ast::YieldExpr& n) override {
        if (n.argument) n.argument->accept(*this);
    }
    void visit(const ast::DynamicImportExpr& n) override {
        if (n.specifier) n.specifier->accept(*this);
    }
    void visit(const ast::DestructuringAssign& n) override {
        visitPattern(n.pattern.get(), /*declareInCurrentScope=*/false, /*isConst=*/false);
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ObjectLit& n) override {
        for (const auto& prop : n.props) {
            if (prop.keyExpr) prop.keyExpr->accept(*this);
            if (prop.value) prop.value->accept(*this);
        }
    }
    void visit(const ast::ArrayLit& n) override {
        for (const auto& el : n.elements) if (el) el->accept(*this);
    }
    void visit(const ast::FunctionExpr& n) override {
        fnStack_.push_back(n.name.empty() ? "<anonymous>" : n.name);
        detFpStack_.push_back(currentDeterministicFp() || hasUseDeterministicPrologue(n.body));
        scopeStack_.emplace_back(/*isFunctionRoot=*/true);
        if (!n.name.empty()) scopeStack_.back().vars[n.name] = VarInfo{true, fnDepth()};
        for (const auto& p : n.params) {
            if (!p.name.empty()) scopeStack_.back().vars[p.name] = VarInfo{false, fnDepth()};
            visitPattern(p.pattern.get(), /*declareInCurrentScope=*/true, /*isConst=*/false);
            if (p.defaultValue) p.defaultValue->accept(*this);
        }
        predeclareFunctionDecls(n.body);
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
        detFpStack_.pop_back();
        fnStack_.pop_back();
    }
    void visit(const ast::ClassExpr& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (m.init) m.init->accept(*this);
            if (m.fn) m.fn->accept(*this);
        }
    }
    void visit(const ast::BlockStmt& n) override {
        scopeStack_.emplace_back();
        predeclareFunctionDecls(n.stmts);
        for (const auto& s : n.stmts) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::VarDecl& n) override {
        if (!n.name.empty()) {
            size_t targetScope = n.isVar ? currentFunctionRootScopeIndex() : (scopeStack_.size() - 1);
            scopeStack_[targetScope].vars[n.name] = VarInfo{n.isConst, fnDepth()};
            recordAliasFromExpr(n.name, n.init.get());
        }
        visitPattern(n.pattern.get(), /*declareInCurrentScope=*/true, n.isConst);
        if (n.init) n.init->accept(*this);
    }
    void visit(const ast::ReturnStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ExprStmt& n) override {
        if (n.expr) n.expr->accept(*this);
    }
    void visit(const ast::IfStmt& n) override {
        if (n.condition) n.condition->accept(*this);
        scopeStack_.emplace_back();
        for (const auto& s : n.thenBody) if (s) s->accept(*this);
        scopeStack_.pop_back();
        scopeStack_.emplace_back();
        for (const auto& s : n.elseBody) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::WhileStmt& n) override {
        if (n.condition) n.condition->accept(*this);
        scopeStack_.emplace_back();
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::DoWhileStmt& n) override {
        scopeStack_.emplace_back();
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
        if (n.condition) n.condition->accept(*this);
    }
    void visit(const ast::ForStmt& n) override {
        scopeStack_.emplace_back();
        for (const auto& s : n.init) if (s) s->accept(*this);
        if (n.condition) n.condition->accept(*this);
        if (n.update) n.update->accept(*this);
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}
    void visit(const ast::DebuggerStmt&) override {}
    void visit(const ast::SwitchStmt& n) override {
        if (n.discriminant) n.discriminant->accept(*this);
        scopeStack_.emplace_back();
        for (const auto& c : n.cases) {
            if (c.test) c.test->accept(*this);
            for (const auto& s : c.body) if (s) s->accept(*this);
        }
        scopeStack_.pop_back();
    }
    void visit(const ast::ForInStmt& n) override {
        emit(DeterministicHazardKind::ObjectIterationOrder, n.span,
             "for..in loop enumerates object properties in insertion-dependent order",
             "iterate over Object.keys(obj).sort() with a deterministic for..of or indexed loop");
        scopeStack_.emplace_back();
        if (!n.name.empty()) scopeStack_.back().vars[n.name] = VarInfo{n.isConst, fnDepth()};
        visitPattern(n.pattern.get(), /*declareInCurrentScope=*/true, n.isConst);
        if (n.object) n.object->accept(*this);
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::LabeledStmt& n) override {
        if (n.body) n.body->accept(*this);
    }
    void visit(const ast::ForOfStmt& n) override {
        scopeStack_.emplace_back();
        if (!n.name.empty()) scopeStack_.back().vars[n.name] = VarInfo{n.isConst, fnDepth()};
        visitPattern(n.pattern.get(), /*declareInCurrentScope=*/true, n.isConst);
        if (n.iterable) n.iterable->accept(*this);
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::TryStmt& n) override {
        scopeStack_.emplace_back();
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
        scopeStack_.emplace_back();
        if (n.hasCatchParam && !n.catchName.empty()) {
            scopeStack_.back().vars[n.catchName] = VarInfo{false, fnDepth()};
        }
        visitPattern(n.catchPattern.get(), /*declareInCurrentScope=*/true, /*isConst=*/false);
        for (const auto& s : n.catchBody) if (s) s->accept(*this);
        scopeStack_.pop_back();
        scopeStack_.emplace_back();
        for (const auto& s : n.finallyBody) if (s) s->accept(*this);
        scopeStack_.pop_back();
    }
    void visit(const ast::ThrowStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ClassDecl& n) override {
        if (!n.name.empty()) scopeStack_.back().vars[n.name] = VarInfo{true, fnDepth()};
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (m.init) m.init->accept(*this);
            if (m.fn) m.fn->accept(*this);
        }
    }
    void visit(const ast::FunctionDecl& n) override {
        if (!n.name.empty()) scopeStack_.back().vars[n.name] = VarInfo{true, fnDepth()};
        fnStack_.push_back(n.name.empty() ? "<anonymous>" : n.name);
        detFpStack_.push_back(currentDeterministicFp() || hasUseDeterministicPrologue(n.body));
        scopeStack_.emplace_back(/*isFunctionRoot=*/true);
        for (const auto& p : n.params) {
            if (!p.name.empty()) scopeStack_.back().vars[p.name] = VarInfo{false, fnDepth()};
            visitPattern(p.pattern.get(), /*declareInCurrentScope=*/true, /*isConst=*/false);
            if (p.defaultValue) p.defaultValue->accept(*this);
        }
        predeclareFunctionDecls(n.body);
        for (const auto& s : n.body) if (s) s->accept(*this);
        scopeStack_.pop_back();
        detFpStack_.pop_back();
        fnStack_.pop_back();
    }
    void visit(const ast::Module& n) override {
        predeclareFunctionDecls(n.body);
        for (const auto& s : n.body) if (s) s->accept(*this);
    }

private:
    struct VarInfo {
        bool isConst = false;
        size_t fnDepth = 0;
    };
    struct ScopeFrame {
        bool isFunctionRoot = false;
        std::unordered_map<std::string, VarInfo> vars;
        explicit ScopeFrame(bool fnRoot = false) : isFunctionRoot(fnRoot) {}
    };

    std::vector<CheckDiagnostic>& diags_;
    std::vector<std::string> fnStack_;
    std::vector<bool> detFpStack_;
    std::vector<ScopeFrame> scopeStack_;
    std::unordered_set<const ast::Call*> sortedObjectCalls_;
    std::unordered_map<std::string, std::string> aliasToGlobal_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> aliasToMethod_;
    std::unordered_set<std::string> emittedExternalVars_;

    size_t fnDepth() const { return fnStack_.size() - 1; }
    bool currentDeterministicFp() const { return !detFpStack_.empty() && detFpStack_.back(); }

    size_t currentFunctionRootScopeIndex() const {
        for (size_t i = scopeStack_.size(); i-- > 0;) {
            if (scopeStack_[i].isFunctionRoot || i == 0) return i;
        }
        return 0;
    }

    void predeclareFunctionDecls(const std::vector<ast::StmtPtr>& stmts) {
        if (scopeStack_.empty()) return;
        for (const auto& s : stmts) {
            if (const auto* fd = ast::exactAs<const ast::FunctionDecl*>(s.get())) {
                if (!fd->name.empty()) {
                    scopeStack_.back().vars[fd->name] = VarInfo{/*isConst=*/true, fnDepth()};
                }
            } else if (const auto* vd = ast::exactAs<const ast::VarDecl*>(s.get())) {
                if (vd->isVar && !vd->name.empty()) {
                    size_t targetScope = currentFunctionRootScopeIndex();
                    scopeStack_[targetScope].vars[vd->name] = VarInfo{/*isConst=*/false, fnDepth()};
                }
            }
        }
    }

    const VarInfo* lookupVar(const std::string& name) const {
        for (size_t i = scopeStack_.size(); i-- > 0;) {
            auto it = scopeStack_[i].vars.find(name);
            if (it != scopeStack_[i].vars.end()) {
                return &it->second;
            }
        }
        return nullptr;
    }

    std::string resolveGlobalObjectExpr(const ast::Expr* expr) const {
        if (!expr) return {};
        if (const auto* id = ast::exactAs<const ast::Ident*>(expr)) {
            auto it = aliasToGlobal_.find(id->name);
            if (it != aliasToGlobal_.end()) return it->second;
            if (lookupVar(id->name) == nullptr) return id->name;
            return {};
        }
        if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(expr)) {
            std::string base = resolveGlobalObjectExpr(mem->object.get());
            if (base == "globalThis") {
                return mem->property;
            }
        }
        return {};
    }

    void recordAliasFromExpr(const std::string& lhsName, const ast::Expr* rhs) {
        if (!rhs) return;
        std::string gObj = resolveGlobalObjectExpr(rhs);
        if (!gObj.empty()) {
            aliasToGlobal_[lhsName] = gObj;
            return;
        }
        if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(rhs)) {
            std::string recvGlobal = resolveGlobalObjectExpr(mem->object.get());
            if (!recvGlobal.empty()) {
                aliasToMethod_[lhsName] = {recvGlobal, mem->property};
            }
        }
    }

    void checkGlobalMemberCall(const std::string& recvName,
                               const std::string& prop,
                               const ast::Call* callNode,
                               Span span) {
        if (recvName == "Math") {
            if (prop == "random") {
                emit(DeterministicHazardKind::UnseededPrng,
                     span,
                     "Math.random() is non-deterministic across lockstep peers and replays",
                     "replace Math.random() with a seeded integer PRNG (e.g. xorshift32 or mulberry32)");
            } else if (isTranscendentalMathMethod(prop)) {
                if (!(currentDeterministicFp() && isDeterministicMathMethod(prop))) {
                    emit(DeterministicHazardKind::TranscendentalFloatDrift,
                         span,
                         "Math." + prop +
                             "() relies on host libm transcendental approximation that can drift by 1 ULP across x86_64 and AArch64",
                         "add \"use deterministic\"; or replace Math." + prop +
                             "() with a deterministic fixed-point LUT");
                }
            }
        } else if (recvName == "Date" && prop == "now") {
            emit(DeterministicHazardKind::WallClockTime,
                 span,
                 "Date.now() reads non-deterministic host wall-clock time",
                 "use a deterministic simulation tick or frame counter");
        } else if (recvName == "performance" && prop == "now") {
            emit(DeterministicHazardKind::WallClockTime,
                 span,
                 "performance.now() reads non-deterministic host high-resolution timer",
                 "use a deterministic simulation tick or frame counter");
        } else if (recvName == "Object" && isObjectEnumerationMethod(prop)) {
            if (sortedObjectCalls_.count(callNode) == 0) {
                emit(DeterministicHazardKind::ObjectIterationOrder,
                     span,
                     "Object." + prop +
                         "() returns keys/entries in insertion-history order without deterministic sorting",
                     "chain .sort() on Object." + prop + "(...) or maintain a sorted array of keys");
            }
        }
    }

    void checkIdentifierAccess(const std::string& name, Span span, bool isWrite) {
        if (fnDepth() == 0) return;  // Only audit inside functions
        const VarInfo* info = lookupVar(name);
        if (info) {
            if (info->fnDepth < fnDepth() && (isWrite || !info->isConst)) {
                std::string key = fnStack_.back() + ":" + name;
                if (emittedExternalVars_.insert(key).second) {
                    emit(DeterministicHazardKind::ExternalStateAccess,
                         span,
                         (isWrite ? "mutation of outer/module variable '" : "read of mutable outer/module variable '") +
                             name + "' violates closed-world function determinism",
                         "pass state explicitly as function arguments and return updated state");
                }
            }
            return;
        }
        // Unbound identifier inside a function:
        if (isWrite || !isWhitelistedGlobalObject(name)) {
            std::string key = fnStack_.back() + ":" + name;
            if (emittedExternalVars_.insert(key).second) {
                emit(DeterministicHazardKind::ExternalStateAccess,
                     span,
                     (isWrite ? "assignment to global '" : "access to external/host global '") +
                         name + "' violates closed-world determinism",
                     "pass all simulation state explicitly as function parameters");
            }
        }
    }

    void checkTargetMutation(const ast::Expr* target, Span span) {
        if (!target || fnDepth() == 0) return;
        const ast::Expr* root = target;
        while (true) {
            if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(root)) {
                root = mem->object.get();
            } else if (const auto* idx = ast::exactAs<const ast::IndexAccess*>(root)) {
                root = idx->object.get();
            } else {
                break;
            }
        }
        if (const auto* id = ast::exactAs<const ast::Ident*>(root)) {
            const VarInfo* info = lookupVar(id->name);
            if (!info || info->fnDepth < fnDepth()) {
                std::string key = fnStack_.back() + ":mut:" + id->name;
                if (emittedExternalVars_.insert(key).second) {
                    emit(DeterministicHazardKind::ExternalStateAccess,
                         span,
                         "mutation of outer/global object '" + id->name +
                             "' violates closed-world function determinism",
                         "only mutate local objects or explicit state parameters");
                }
            }
        }
    }

    void visitPattern(const ast::BindingPattern* pat, bool declareInCurrentScope, bool isConst) {
        if (!pat) return;
        for (const auto& el : pat->elements) {
            if (declareInCurrentScope && !el.name.empty() && !scopeStack_.empty()) {
                scopeStack_.back().vars[el.name] = VarInfo{isConst, fnDepth()};
            }
            visitPattern(el.pattern.get(), declareInCurrentScope, isConst);
            if (el.target) el.target->accept(*this);
            if (el.keyExpr) el.keyExpr->accept(*this);
            if (el.defaultValue) el.defaultValue->accept(*this);
        }
    }

    void emit(DeterministicHazardKind kind,
              Span span,
              std::string message,
              std::string remediation) {
        CheckDiagnostic d;
        d.kind = kind;
        d.span = span;
        d.functionName = fnStack_.empty() ? "<top-level>" : fnStack_.back();
        d.message = std::move(message);
        d.remediation = std::move(remediation);
        diags_.push_back(std::move(d));
    }
};

// Audits the lowered IL module for closed-world determinism:
// 1. Tracks global object and member aliases across SSA values and module env slots.
// 2. Audits IL-level MathUnary/Pow, outer/module EnvGet/EnvSet, and external GlobalGet.
// 3. Propagates non-determinism hazards across the IL direct/closure call graph so
//    any function calling a non-deterministic helper is also rejected.
void auditIlClosedWorld(const il::Module& ilMod, std::vector<CheckDiagnostic>& diags) {
    auto keyName = [&](uint32_t idx) -> std::string {
        if (idx < ilMod.keyConstants.size()) return ilMod.keyConstants[idx];
        return {};
    };

    auto hasDiag = [&](const std::string& fnName, DeterministicHazardKind kind) {
        for (const auto& d : diags) {
            if (d.functionName == fnName && d.kind == kind) return true;
        }
        return false;
    };

    auto emitIl = [&](const std::string& fnName,
                      DeterministicHazardKind kind,
                      Span span,
                      std::string msg,
                      std::string help) {
        if (!hasDiag(fnName, kind)) {
            diags.push_back(CheckDiagnostic{kind, span, fnName, std::move(msg), std::move(help)});
        }
    };

    // Pass 1: Track module-level environment slot aliases initialized in `main`.
    std::unordered_map<uint32_t, std::string> moduleSlotGlobal;
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> moduleSlotMember;
    for (const auto& fn : ilMod.functions) {
        if (!fn.isEntryPoint && fn.name != "main") continue;
        std::unordered_map<il::ValueId, std::string> valGlobal;
        std::unordered_map<il::ValueId, std::pair<std::string, std::string>> valMember;
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::GlobalGet || inst.op == il::Op::ResolveName) {
                    std::string g = keyName(inst.keyIndex);
                    if (!g.empty() && inst.result != il::kNoValue) valGlobal[inst.result] = g;
                } else if (inst.op == il::Op::PropGet && !inst.operands.empty() &&
                           inst.result != il::kNoValue) {
                    auto itG = valGlobal.find(inst.operands[0]);
                    std::string prop = keyName(inst.keyIndex);
                    if (itG != valGlobal.end() && !prop.empty()) {
                        if (itG->second == "globalThis") valGlobal[inst.result] = prop;
                        else valMember[inst.result] = {itG->second, prop};
                    }
                } else if (inst.op == il::Op::EnvSet && inst.operands.size() >= 2) {
                    il::ValueId rhs = inst.operands[1];
                    if (auto itG = valGlobal.find(rhs); itG != valGlobal.end())
                        moduleSlotGlobal[inst.envIndex] = itG->second;
                    if (auto itM = valMember.find(rhs); itM != valMember.end())
                        moduleSlotMember[inst.envIndex] = itM->second;
                }
            }
        }
    }

    // Pass 2: Audit each IL function and build call graph edges (caller -> callee).
    std::unordered_map<std::string, std::vector<std::pair<std::string, Span>>> callerToCallees;

    for (const auto& fn : ilMod.functions) {
        if (fn.isExternal) continue;
        const bool isTopLevel = fn.isEntryPoint || fn.name == "main";
        std::unordered_map<il::ValueId, std::string> valGlobal;
        std::unordered_map<il::ValueId, std::pair<std::string, std::string>> valMember;
        std::unordered_set<il::ValueId> moduleEnvIds;

        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                switch (inst.op) {
                    case il::Op::ModuleEnvGet:
                        if (inst.result != il::kNoValue) moduleEnvIds.insert(inst.result);
                        break;
                    case il::Op::GlobalGet:
                    case il::Op::ResolveName: {
                        std::string g = keyName(inst.keyIndex);
                        if (!g.empty() && inst.result != il::kNoValue) valGlobal[inst.result] = g;
                        if (!isTopLevel && !g.empty() && !isWhitelistedGlobalObject(g)) {
                            emitIl(fn.name, DeterministicHazardKind::ExternalStateAccess, inst.span,
                                   "IL global access to '" + g + "' violates closed-world determinism",
                                   "pass external state explicitly via function parameters");
                        }
                        break;
                    }
                    case il::Op::EnvGet:
                    case il::Op::EnvGetTdz: {
                        bool isModEnv = !fn.needsEnv ||
                                        (!inst.operands.empty() && moduleEnvIds.count(inst.operands[0]) > 0);
                        if (isModEnv && inst.envDepth == 0 && inst.result != il::kNoValue) {
                            if (auto itG = moduleSlotGlobal.find(inst.envIndex); itG != moduleSlotGlobal.end())
                                valGlobal[inst.result] = itG->second;
                            if (auto itM = moduleSlotMember.find(inst.envIndex); itM != moduleSlotMember.end())
                                valMember[inst.result] = itM->second;
                        }
                        if (!isTopLevel && (isModEnv || inst.envDepth > 0) && !inst.envImmutable) {
                            emitIl(fn.name, DeterministicHazardKind::ExternalStateAccess, inst.span,
                                   "IL read from mutable outer/module environment slot violates closed-world determinism",
                                   "pass mutable state explicitly as function arguments");
                        }
                        break;
                    }
                    case il::Op::EnvSet: {
                        bool isModEnv = !fn.needsEnv ||
                                        (!inst.operands.empty() && moduleEnvIds.count(inst.operands[0]) > 0);
                        if (!isTopLevel && (isModEnv || inst.envDepth > 0)) {
                            emitIl(fn.name, DeterministicHazardKind::ExternalStateAccess, inst.span,
                                   "IL write to outer/module environment slot violates closed-world determinism",
                                   "do not mutate outer or module-scoped bindings inside deterministic functions");
                        }
                        break;
                    }
                    case il::Op::PropGet: {
                        if (!inst.operands.empty() && inst.result != il::kNoValue) {
                            auto itG = valGlobal.find(inst.operands[0]);
                            std::string prop = keyName(inst.keyIndex);
                            if (itG != valGlobal.end() && !prop.empty()) {
                                if (itG->second == "globalThis") valGlobal[inst.result] = prop;
                                else valMember[inst.result] = {itG->second, prop};
                            }
                        }
                        break;
                    }
                    case il::Op::PropSet: {
                        if (!isTopLevel && !inst.operands.empty()) {
                            if (auto itG = valGlobal.find(inst.operands[0]); itG != valGlobal.end()) {
                                emitIl(fn.name, DeterministicHazardKind::ExternalStateAccess, inst.span,
                                       "IL property write to global '" + itG->second + "' violates closed-world determinism",
                                       "only mutate local objects or explicit state parameters");
                            }
                        }
                        break;
                    }
                    case il::Op::MethodCall: {
                        if (!inst.operands.empty()) {
                            auto itG = valGlobal.find(inst.operands[0]);
                            std::string prop = keyName(inst.keyIndex);
                            if (itG != valGlobal.end() && !prop.empty()) {
                                if (itG->second == "Math" && prop == "random") {
                                    emitIl(fn.name, DeterministicHazardKind::UnseededPrng, inst.span,
                                           "Math.random() via alias is non-deterministic across lockstep peers",
                                           "replace Math.random() with a seeded integer PRNG");
                                } else if ((itG->second == "Date" || itG->second == "performance") && prop == "now") {
                                    emitIl(fn.name, DeterministicHazardKind::WallClockTime, inst.span,
                                           itG->second + ".now() reads non-deterministic host wall-clock time",
                                           "use a deterministic simulation tick or frame counter");
                                }
                            }
                        }
                        break;
                    }
                    case il::Op::DynamicCall: {
                        if (!inst.operands.empty()) {
                            if (auto itM = valMember.find(inst.operands[0]); itM != valMember.end()) {
                                const auto& [gObj, prop] = itM->second;
                                if (gObj == "Math" && prop == "random") {
                                    emitIl(fn.name, DeterministicHazardKind::UnseededPrng, inst.span,
                                           "aliased Math.random() call is non-deterministic",
                                           "replace Math.random() with a seeded integer PRNG");
                                } else if ((gObj == "Date" || gObj == "performance") && prop == "now") {
                                    emitIl(fn.name, DeterministicHazardKind::WallClockTime, inst.span,
                                           "aliased " + gObj + ".now() call reads wall-clock time",
                                           "use a deterministic simulation tick or frame counter");
                                }
                            }
                        }
                        break;
                    }
                    case il::Op::Construct: {
                        if (inst.operands.size() == 1) {
                            if (auto itG = valGlobal.find(inst.operands[0]);
                                itG != valGlobal.end() && itG->second == "Date") {
                                emitIl(fn.name, DeterministicHazardKind::WallClockTime, inst.span,
                                       "new Date() via alias captures host wall-clock time",
                                       "pass an explicit deterministic epoch timestamp");
                            }
                        }
                        break;
                    }
                    case il::Op::MathUnary: {
                        auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                        if ((kind == il::MathUnaryFn::Sin || kind == il::MathUnaryFn::Cos) &&
                            !fn.isDeterministicFp()) {
                            emitIl(fn.name, DeterministicHazardKind::TranscendentalFloatDrift, inst.span,
                                   "IL math.unary(sin/cos) without \"use deterministic\" may drift across platforms",
                                   "add \"use deterministic\"; or use fixed-point arithmetic");
                        }
                        break;
                    }
                    case il::Op::Call: {
                        if (inst.calleeIndex < ilMod.functions.size()) {
                            callerToCallees[fn.name].push_back(
                                {ilMod.functions[inst.calleeIndex].name, inst.span});
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    // Pass 3: Transitive propagation of hazards along the IL call graph.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [caller, callees] : callerToCallees) {
            if (caller == "main") continue;
            for (const auto& [callee, callSpan] : callees) {
                if (callee == caller) continue;
                std::vector<CheckDiagnostic> toAdd;
                for (const auto& d : diags) {
                    if (d.functionName == callee && !hasDiag(caller, d.kind)) {
                        CheckDiagnostic prop = d;
                        prop.functionName = caller;
                        prop.span = callSpan;
                        prop.message = "transitive call to non-deterministic function '" + callee +
                                       "': " + d.message;
                        toAdd.push_back(std::move(prop));
                    }
                }
                if (!toAdd.empty()) {
                    for (auto& p : toAdd) diags.push_back(std::move(p));
                    changed = true;
                }
            }
        }
    }
}

}  // namespace

const char* deterministicHazardTag(DeterministicHazardKind kind) {
    switch (kind) {
        case DeterministicHazardKind::UnseededPrng:
            return "unseeded-prng";
        case DeterministicHazardKind::WallClockTime:
            return "wall-clock-time";
        case DeterministicHazardKind::TranscendentalFloatDrift:
            return "transcendental-float-drift";
        case DeterministicHazardKind::ObjectIterationOrder:
            return "object-iteration-order";
        case DeterministicHazardKind::CrossArchBitwiseDrift:
            return "cross-arch-bitwise-drift";
        case DeterministicHazardKind::ExternalStateAccess:
            return "external-state-access";
    }
    return "deterministic-hazard";
}

void checkDeterministicLockstep(const ast::Program& prog,
                                const il::Module& ilMod,
                                std::vector<CheckDiagnostic>& diags) {
    const bool moduleDetFp = ilMod.hasDeterministicFp() || hasUseDeterministicPrologue(prog.body);
    DeterministicAstVisitor visitor(diags, moduleDetFp);
    prog.accept(visitor);
    auditIlClosedWorld(ilMod, diags);
}

bool checkDeterministicSource(std::string_view src,
                              std::vector<CheckDiagnostic>& diags,
                              std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add("input.js", std::string(src));
    DiagnosticSink parseDiags;
    auto tokens = Lexer(buf, parseDiags).lex();
    if (parseDiags.hasErrors()) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }
    auto astMod = Parser(std::move(tokens), parseDiags).parseModule("input.js");
    if (parseDiags.hasErrors() || !astMod) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }
    auto inferred = types::inferModule(*astMod, parseDiags);
    if (parseDiags.hasErrors() || !inferred) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }
    auto ilMod = lower::lowerModule(*astMod, parseDiags, &*inferred, nullptr, &sources);
    if (parseDiags.hasErrors() || !ilMod) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }

    checkDeterministicLockstep(*astMod, *ilMod, diags);
    return true;
}

std::string renderDeterministicDiagnostics(const std::vector<CheckDiagnostic>& diags,
                                           const SourceBuffer& buffer) {
    std::ostringstream os;
    for (const auto& d : diags) {
        auto lc = buffer.lineCol(d.span.begin);
        os << buffer.name() << ":" << lc.line << ":" << lc.column
           << ": warning [" << deterministicHazardTag(d.kind) << "] (in "
           << d.functionName << "): " << d.message << "\n"
           << "  help: " << d.remediation << "\n";
    }
    os << "bronze check --deterministic: " << diags.size() << " lockstep hazard"
       << (diags.size() == 1 ? "" : "s") << " found\n";
    return os.str();
}

std::string renderDeterministicDiagnostics(const std::vector<CheckDiagnostic>& diags,
                                           const SourceSet& sources) {
    std::ostringstream os;
    for (const auto& d : diags) {
        const SourceBuffer& buf = sources.at(d.span.file);
        auto lc = buf.lineCol(d.span.begin);
        os << buf.name() << ":" << lc.line << ":" << lc.column
           << ": warning [" << deterministicHazardTag(d.kind) << "] (in "
           << d.functionName << "): " << d.message << "\n"
           << "  help: " << d.remediation << "\n";
    }
    os << "bronze check --deterministic: " << diags.size() << " lockstep hazard"
       << (diags.size() == 1 ? "" : "s") << " found\n";
    return os.str();
}

int runDeterministicCheckFile(const std::string& sourcePath,
                              std::string* outString,
                              const std::vector<modules::ModuleRoot>& moduleRoots,
                              const std::string& importMapPath) {
    SourceSet sources;
    DiagnosticSink parseDiags;
    auto astMod = modules::loadProgram(sourcePath, sources, parseDiags, {moduleRoots, importMapPath});
    if (parseDiags.hasErrors() || !astMod) {
        std::string msg = parseDiags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto inferred = types::inferModule(*astMod, parseDiags);
    if (parseDiags.hasErrors() || !inferred) {
        std::string msg = parseDiags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto ilMod = lower::lowerModule(*astMod, parseDiags, &*inferred, nullptr, &sources);
    if (parseDiags.hasErrors() || !ilMod) {
        std::string msg = parseDiags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::vector<CheckDiagnostic> lockstepDiags;
    checkDeterministicLockstep(*astMod, *ilMod, lockstepDiags);
    std::string rendered = renderDeterministicDiagnostics(lockstepDiags, sources);
    if (outString) {
        *outString = rendered;
    } else {
        std::fputs(rendered.c_str(), stdout);
    }
    return lockstepDiags.empty() ? 0 : 1;
}

}  // namespace bronze::check
