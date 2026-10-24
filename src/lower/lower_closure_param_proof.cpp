// The closure PARAMETER proof: which parameters of a nested `function f() {}`
// are Numbers at every call, the one walk of the enclosing scope that decides
// it, and the point at which the answer reaches the declaration's IL skeleton.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "ast/query_walk.h"
#include "lower/lowerer.h"

namespace bronze::lower {

bool Lowerer::closureParamProofDisabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_CLOSURE_PARAM_PROOF");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return disabled;
}

namespace {

struct ScopeFrame {
    const std::vector<ast::StmtPtr>* stmts = nullptr;
    const std::vector<ast::Param>* params = nullptr;
};

struct CallSiteWithScopes {
    const ast::Call* call = nullptr;
    std::vector<ScopeFrame> scopes;
};

std::string matchGlobalThisFnExport(const ast::Stmt& stmt) {
    if (const auto* blk = dynamic_cast<const ast::BlockStmt*>(&stmt)) {
        if (blk->stmts.size() == 1 && blk->stmts[0]) {
            return matchGlobalThisFnExport(*blk->stmts[0]);
        }
        return {};
    }
    const auto* exprStmt = dynamic_cast<const ast::ExprStmt*>(&stmt);
    if (!exprStmt || !exprStmt->expr) return {};
    const auto* bin = dynamic_cast<const ast::Binary*>(exprStmt->expr.get());
    if (!bin || bin->op != ast::BinaryOp::Assign) return {};
    const auto* member = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get());
    if (!member || member->optional || member->isPrivate || member->property.empty()) return {};
    const auto* base = dynamic_cast<const ast::Ident*>(member->object.get());
    if (!base || base->name != "globalThis") return {};
    const auto* rhs = dynamic_cast<const ast::Ident*>(bin->rhs.get());
    if (!rhs || rhs->name != member->property) return {};
    return member->property;
}

bool isKnownTypedArrayCtorName(const std::string& name) {
    return name == "Float64Array" || name == "Float32Array" || name == "Int32Array" ||
           name == "Uint32Array" || name == "Int16Array" || name == "Uint16Array" ||
           name == "Int8Array" || name == "Uint8Array" || name == "Uint8ClampedArray";
}

class ModuleDetachHazardScan final : public ast::detail::IdentVisitor {
public:
    using ast::detail::IdentVisitor::visit;
    bool hazard = false;

    void visit(const ast::Ident& i) override {
        if (i.name == "eval" || i.name == "postMessage" || i.name == "structuredClone" ||
            i.name == "ArrayBuffer" || i.name == "SharedArrayBuffer") {
            hazard = true;
        }
    }
    void visit(const ast::MemberAccess& m) override {
        if (m.property == "transfer" || m.property == "transferToFixedLength" ||
            m.property == "resize" || m.property == "grow" || m.property == "detach") {
            hazard = true;
        }
        ast::detail::IdentVisitor::visit(m);
    }
};

// Every mention of one name under a function body, split into the two things a
// mention can be: the CALLEE of an ordinary call — whose argument list is then
// evidence about the parameters — and anything else at all, which is the
// function value leaving through a door this analysis cannot follow.
class CalleeOnlyScan final : public ast::detail::IdentVisitor {
public:
    explicit CalleeOnlyScan(std::string name, const ast::FunctionDecl* targetDecl = nullptr)
        : name_(std::move(name)), targetDecl_(targetDecl) {}

    std::vector<const ast::Call*> sites;
    std::vector<CallSiteWithScopes> scopedSites;
    std::vector<ScopeFrame> scopeStack;
    bool escaped = false;
    bool spread = false;

    void visit(const ast::Call& c) override {
        const auto* id = dynamic_cast<const ast::Ident*>(c.callee.get());
        if (id != nullptr && id->name == name_) {
            for (const auto& arg : c.args) {
                if (dynamic_cast<const ast::SpreadElement*>(arg.get()) != nullptr) spread = true;
            }
            sites.push_back(&c);
            scopedSites.push_back(CallSiteWithScopes{&c, scopeStack});
        } else {
            c.callee->accept(*this);
        }
        for (const auto& arg : c.args) arg->accept(*this);
    }

    void visit(const ast::BlockStmt& b) override {
        scopeStack.push_back(ScopeFrame{&b.stmts, nullptr});
        for (const auto& s : b.stmts) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
    }

    void visit(const ast::ForStmt& f) override {
        for (const auto& s : f.init) {
            if (s) s->accept(*this);
        }
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        scopeStack.push_back(ScopeFrame{&f.body, nullptr});
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
    }

    void visit(const ast::WhileStmt& w) override {
        if (w.condition) w.condition->accept(*this);
        scopeStack.push_back(ScopeFrame{&w.body, nullptr});
        for (const auto& s : w.body) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
    }

    void visit(const ast::DoWhileStmt& d) override {
        scopeStack.push_back(ScopeFrame{&d.body, nullptr});
        for (const auto& s : d.body) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
        if (d.condition) d.condition->accept(*this);
    }

    void visit(const ast::IfStmt& i) override {
        if (i.condition) i.condition->accept(*this);
        scopeStack.push_back(ScopeFrame{&i.thenBody, nullptr});
        for (const auto& s : i.thenBody) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
        if (!i.elseBody.empty()) {
            scopeStack.push_back(ScopeFrame{&i.elseBody, nullptr});
            for (const auto& s : i.elseBody) {
                if (s) s->accept(*this);
            }
            scopeStack.pop_back();
        }
    }

    void visit(const ast::FunctionDecl& f) override {
        if (targetDecl_ != nullptr && &f != targetDecl_ && f.name == name_) {
            escaped = true;
        }
        for (const auto& p : f.params) {
            if (!p.pattern && p.name == name_) escaped = true;
        }
        ast::detail::visitParamExprs(f.params, *this);
        scopeStack.push_back(ScopeFrame{&f.body, &f.params});
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
    }

    void visit(const ast::FunctionExpr& f) override {
        if (f.name == name_) escaped = true;
        for (const auto& p : f.params) {
            if (!p.pattern && p.name == name_) escaped = true;
        }
        ast::detail::visitParamExprs(f.params, *this);
        scopeStack.push_back(ScopeFrame{&f.body, &f.params});
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
        scopeStack.pop_back();
    }

    void visit(const ast::Ident& i) override {
        if (i.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(i);
    }

    void visit(const ast::VarDecl& v) override {
        if (!v.pattern && v.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(v);
    }

private:
    std::string name_;
    const ast::FunctionDecl* targetDecl_ = nullptr;
};

struct ConstLookupResult {
    const ast::Expr* init = nullptr;
    std::vector<ScopeFrame> scopes;
};

ConstLookupResult findConstInitInScopes(const std::string& name,
                                        const std::vector<ScopeFrame>& scopes) {
    if (name.empty()) return {};
    for (size_t idx = scopes.size(); idx > 0; --idx) {
        const ScopeFrame& frame = scopes[idx - 1];
        if (frame.params != nullptr) {
            for (const auto& p : *frame.params) {
                if (p.pattern) {
                    for (const auto& n : ast::patternBoundNames(*p.pattern)) {
                        if (n == name) return {};
                    }
                } else if (p.name == name) {
                    return {};
                }
            }
        }
        const auto* stmts = frame.stmts;
        if (stmts == nullptr) continue;
        int matchCount = 0;
        const ast::VarDecl* matchedDecl = nullptr;
        for (const auto& s : *stmts) {
            if (!s) continue;
            if (const auto* vd = dynamic_cast<const ast::VarDecl*>(s.get())) {
                if (vd->pattern) {
                    for (const auto& n : ast::patternBoundNames(*vd->pattern)) {
                        if (n == name) ++matchCount;
                    }
                } else if (vd->name == name) {
                    ++matchCount;
                    matchedDecl = vd;
                }
            } else if (const auto* fd = dynamic_cast<const ast::FunctionDecl*>(s.get())) {
                if (fd->name == name) ++matchCount;
            } else if (const auto* cd = dynamic_cast<const ast::ClassDecl*>(s.get())) {
                if (cd->name == name) ++matchCount;
            }
        }
        if (matchCount > 0) {
            if (matchCount == 1 && matchedDecl != nullptr && matchedDecl->isConst &&
                !matchedDecl->isVar && matchedDecl->init != nullptr) {
                std::vector<ScopeFrame> declScopes(
                    scopes.begin(), scopes.begin() + static_cast<std::ptrdiff_t>(idx));
                return ConstLookupResult{matchedDecl->init.get(), std::move(declScopes)};
            }
            return {};
        }
    }
    return {};
}

std::optional<int64_t> evalConstIntInScopes(const ast::Expr& e,
                                            const std::vector<ScopeFrame>& scopes, int depth) {
    if (depth <= 0) return std::nullopt;
    if (const auto* n = dynamic_cast<const ast::NumberLit*>(&e)) {
        if (!std::isfinite(n->value) || std::floor(n->value) != n->value ||
            (n->value == 0.0 && std::signbit(n->value)) ||
            n->value < -2147483648.0 || n->value > 2147483647.0) {
            return std::nullopt;
        }
        return static_cast<int64_t>(n->value);
    }
    if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) {
        if (!u->operand) return std::nullopt;
        auto inner = evalConstIntInScopes(*u->operand, scopes, depth - 1);
        if (!inner) return std::nullopt;
        if (u->op == ast::UnaryOp::Posate) return *inner;
        if (u->op == ast::UnaryOp::Negate) {
            if (*inner == 0 || *inner == -2147483648LL) return std::nullopt;
            return -*inner;
        }
        if (u->op == ast::UnaryOp::BitNot) {
            return static_cast<int64_t>(~static_cast<int32_t>(*inner));
        }
        return std::nullopt;
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(&e)) {
        if (!b->lhs || !b->rhs) return std::nullopt;
        auto l = evalConstIntInScopes(*b->lhs, scopes, depth - 1);
        auto r = evalConstIntInScopes(*b->rhs, scopes, depth - 1);
        if (!l || !r) return std::nullopt;
        switch (b->op) {
            case ast::BinaryOp::Add: {
                const int64_t v = *l + *r;
                if (v < -2147483648LL || v > 2147483647LL) return std::nullopt;
                return v;
            }
            case ast::BinaryOp::Sub: {
                const int64_t v = *l - *r;
                if (v < -2147483648LL || v > 2147483647LL) return std::nullopt;
                return v;
            }
            case ast::BinaryOp::Mul: {
                if ((*l == 0 && *r < 0) || (*r == 0 && *l < 0)) return std::nullopt;
                const int64_t v = *l * *r;
                if (v < -2147483648LL || v > 2147483647LL) return std::nullopt;
                return v;
            }
            case ast::BinaryOp::BitAnd:
                return static_cast<int64_t>(static_cast<int32_t>(*l) & static_cast<int32_t>(*r));
            case ast::BinaryOp::BitOr:
                return static_cast<int64_t>(static_cast<int32_t>(*l) | static_cast<int32_t>(*r));
            case ast::BinaryOp::BitXor:
                return static_cast<int64_t>(static_cast<int32_t>(*l) ^ static_cast<int32_t>(*r));
            case ast::BinaryOp::Shl:
                return static_cast<int64_t>(static_cast<int32_t>(
                    static_cast<uint32_t>(static_cast<int32_t>(*l))
                    << (static_cast<uint32_t>(static_cast<int32_t>(*r)) & 31u)));
            case ast::BinaryOp::Shr:
                return static_cast<int64_t>(
                    static_cast<int32_t>(*l) >>
                    (static_cast<uint32_t>(static_cast<int32_t>(*r)) & 31u));
            case ast::BinaryOp::UShr: {
                const uint32_t uval =
                    static_cast<uint32_t>(static_cast<int32_t>(*l)) >>
                    (static_cast<uint32_t>(static_cast<int32_t>(*r)) & 31u);
                if (uval > 2147483647u) return std::nullopt;
                return static_cast<int64_t>(uval);
            }
            default:
                return std::nullopt;
        }
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
        auto found = findConstInitInScopes(id->name, scopes);
        if (!found.init) return std::nullopt;
        return evalConstIntInScopes(*found.init, found.scopes, depth - 1);
    }
    return std::nullopt;
}

std::optional<int64_t> evalTypedArrayConstLenInScopes(
    const ast::Expr& e, const std::vector<ScopeFrame>& scopes,
    const std::unordered_set<std::string>& rebound,
    const std::unordered_set<std::string>& hostGlobals, int depth) {
    if (depth <= 0) return std::nullopt;
    if (const auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (!ne->callee || ne->args.size() != 1 || !ne->args[0]) return std::nullopt;
        if (dynamic_cast<const ast::SpreadElement*>(ne->args[0].get()) != nullptr) {
            return std::nullopt;
        }
        const auto* ctorId = dynamic_cast<const ast::Ident*>(ne->callee.get());
        if (!ctorId || !isKnownTypedArrayCtorName(ctorId->name)) return std::nullopt;
        if (rebound.contains(ctorId->name) || hostGlobals.contains(ctorId->name)) {
            return std::nullopt;
        }
        auto len = evalConstIntInScopes(*ne->args[0], scopes, 8);
        if (!len || *len < 0 || *len > 2147483647LL) return std::nullopt;
        return *len;
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
        auto found = findConstInitInScopes(id->name, scopes);
        if (!found.init) return std::nullopt;
        return evalTypedArrayConstLenInScopes(*found.init, found.scopes, rebound, hostGlobals,
                                              depth - 1);
    }
    return std::nullopt;
}

}  // namespace

void Lowerer::initModuleBufferDetachFree() {
    moduleBufferDetachFree_ = true;
    ModuleDetachHazardScan scan;
    for (const auto& s : astModule_.body) {
        if (s) s->accept(scan);
        if (scan.hazard) {
            moduleBufferDetachFree_ = false;
            return;
        }
    }
}

void Lowerer::planFunctionParamFacts(const std::vector<ast::Param>& params,
                                     const std::vector<ast::StmtPtr>& stmts) {
    if (closureParamProofDisabled() || fnParamFactsStack_.empty()) return;

    const bool isTopLevel = (&stmts == &astModule_.body);
    if (isTopLevel && inference_ == nullptr) return;

    bool anyDeclaration = false;
    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl != nullptr && !fnDecl->isGenerator && !fnDecl->isAsync &&
            !fnDecl->params.empty()) {
            anyDeclaration = true;
            break;
        }
    }
    if (!anyDeclaration) return;

    std::unordered_set<std::string> rebound = ast::getDeeplyReboundNames(astModule_.body);
    if (!isTopLevel) {
        for (auto& name : ast::getDeeplyReboundNames(stmts)) rebound.insert(std::move(name));
    }
    for (const auto& p : params) {
        if (!p.pattern && !p.name.empty()) rebound.insert(p.name);
        if (!p.defaultValue) continue;
        for (auto& name : ast::getDeeplyReboundNames(*p.defaultValue)) {
            rebound.insert(std::move(name));
        }
    }

    uint32_t moduleFnIndex = 0;
    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl == nullptr) continue;
        const uint32_t thisFnIdx = moduleFnIndex++;
        if (fnDecl->isGenerator || fnDecl->isAsync || fnDecl->isExported) continue;
        if (fnDecl->name.empty() || fnDecl->params.empty()) continue;
        if (isTopLevel && !inference_->isDirectCallable(thisFnIdx)) continue;
        if (rebound.contains(fnDecl->name)) continue;

        bool shapeFits = true;
        for (const auto& p : fnDecl->params) {
            if (p.defaultValue || p.isRest || p.pattern || p.name.empty()) shapeFits = false;
        }
        if (!shapeFits) continue;
        // `arguments` is a door the call-site scan cannot see through: the
        // body calls itself by `arguments.callee(...)` with arguments no site
        // shows, and a sloppy `arguments[k] = v` writes the parameter itself.
        if (ast::usesArguments(fnDecl->params, fnDecl->body)) continue;

        CalleeOnlyScan scan(fnDecl->name, fnDecl);
        if (!isTopLevel) scan.scopeStack.push_back(ScopeFrame{&astModule_.body, nullptr});
        scan.scopeStack.push_back(ScopeFrame{&stmts, isTopLevel ? nullptr : &params});
        for (const auto& s : stmts) {
            if (!s) continue;
            if (isTopLevel && matchGlobalThisFnExport(*s) == fnDecl->name) continue;
            s->accept(scan);
        }
        for (const auto& p : params) {
            if (p.defaultValue) p.defaultValue->accept(scan);
            if (p.pattern) ast::detail::visitPatternExprs(p.pattern.get(), scan);
        }
        scan.scopeStack.pop_back();
        if (!isTopLevel) scan.scopeStack.pop_back();

        if (scan.escaped || scan.spread || scan.scopedSites.empty()) continue;

        FnParamFacts facts;
        const std::unordered_set<std::string> bodyAssigned = ast::getAssignedNames(fnDecl->body);
        for (size_t k = 0; k < fnDecl->params.size(); ++k) {
            const std::string& pName = fnDecl->params[k].name;
            if (bodyAssigned.contains(pName)) continue;

            bool allInt = true;
            int64_t minInt = std::numeric_limits<int64_t>::max();
            int64_t maxInt = std::numeric_limits<int64_t>::min();
            for (const auto& site : scan.scopedSites) {
                if (k >= site.call->args.size() || !site.call->args[k]) {
                    allInt = false;
                    break;
                }
                auto v = evalConstIntInScopes(*site.call->args[k], site.scopes, 8);
                if (!v) {
                    allInt = false;
                    break;
                }
                minInt = std::min(minInt, *v);
                maxInt = std::max(maxInt, *v);
            }
            if (allInt && minInt <= maxInt) {
                facts.intParamBounds[pName] = {minInt, maxInt};
                if (minInt == maxInt) {
                    facts.constIntParams[pName] = minInt;
                }
            }

            if (moduleBufferDetachFree_ && !typedElemDisabled_) {
                bool allTa = true;
                int64_t minLen = std::numeric_limits<int64_t>::max();
                int64_t maxLen = std::numeric_limits<int64_t>::min();
                for (const auto& site : scan.scopedSites) {
                    if (k >= site.call->args.size() || !site.call->args[k]) {
                        allTa = false;
                        break;
                    }
                    auto len = evalTypedArrayConstLenInScopes(
                        *site.call->args[k], site.scopes, rebound, hostGlobals_, 4);
                    if (!len) {
                        allTa = false;
                        break;
                    }
                    minLen = std::min(minLen, *len);
                    maxLen = std::max(maxLen, *len);
                }
                if (allTa && minLen <= maxLen) {
                    facts.minTypedArrayLenParams[pName] = minLen;
                    facts.maxTypedArrayLenParams[pName] = maxLen;
                    if (minLen == maxLen) {
                        facts.exactTypedArrayLenParams[pName] = minLen;
                    }
                }
            }
        }
        if (!facts.constIntParams.empty() || !facts.intParamBounds.empty() ||
            !facts.minTypedArrayLenParams.empty()) {
            fnParamFactsStack_.back().emplace(fnDecl, std::move(facts));
        }
    }
}

// THE PARAMETER PROOF FOR A CLOSURE, and the general form of what `param
// f(x): number` does in a manifest by hand.
//
// Stage E3 wrote down the gap: a class method's parameters are typed by joining
// what every call site passes (types/flow_expr.cpp, `contributeArgs`), a module
// function's by the same join over its enumerated callers (types/infer.cpp),
// and a nested `function f() {}` by nothing — it is reached through a function
// value, so inference has no `functionIndex` for it and no signature that can
// speak for its result or its arguments. `bench/pins/env-slot-kernel.pins`
// carries five `param` lines that exist only because of that hole.
//
// But a nested declaration's callers ARE enumerable, on exactly the terms the
// static call plan (lower_static_call_plan.cpp) already establishes for its
// VALUE. The declaration is
// the value, it is installed before the scope's first statement runs, and the
// scope's whole lexical reach is one subtree this compilation holds. So: walk
// that subtree once, and if every mention of the name is the callee of an
// ordinary call, those calls are ALL the calls, and the join over their
// arguments at position k is a fact about every value parameter k will ever
// hold — the same join, over the same lattice, that the two paths above run.
//
// This is a PROOF and not a pin. Every clause below is a refusal in the safe
// direction, and a refusal costs the f64 slot and nothing else:
//
//   - a plain declaration only: a generator or an async function's parameters
//     are bound by a resume edge, not by the call.
//   - no default, rest or pattern parameter, because the value bound is then
//     not the value passed — the same three positions `applySignaturePins`
//     refuses, and for the same reason: there is no `undefined` in an f64.
//   - the name is not rebound anywhere in the subtree (`getDeeplyAssignedNames`,
//     the stable-plan's test), and every mention of it is a callee.
//   - no spread at any site.
//   - every site supplies position k, and the argument there is a proven
//     Number. A SHORT call binds `undefined`, which is not a Number, so a
//     single one refuses the position.
//
// What it deliberately does NOT reach: a closure that escapes. `env_slot_kernel`
// ends `return render;`, so `render`'s own parameter stays unproven and stays
// pinned — the manifest says so. That is the honest boundary of this mechanism:
// it types the closures a factory calls, not the closure the factory hands out.
void Lowerer::planClosureParamNumbers(const std::vector<ast::Param>& params,
                                      const std::vector<ast::StmtPtr>& stmts) {
    if (inference_ == nullptr || closureParamProofDisabled()) return;
    // `lowerFunctionBody` opened this body's frame before calling in.
    if (provenClosureParams_.empty()) return;

    bool anyDeclaration = false;
    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl != nullptr && !fnDecl->isGenerator && !fnDecl->isAsync &&
            !fnDecl->params.empty()) {
            anyDeclaration = true;
            break;
        }
    }
    if (!anyDeclaration) return;

    std::unordered_set<std::string> rebound = ast::getDeeplyAssignedNames(stmts);
    for (const auto& p : params) {
        if (!p.defaultValue) continue;
        for (auto& name : ast::getDeeplyAssignedNames(*p.defaultValue)) {
            rebound.insert(std::move(name));
        }
    }

    struct CandidateFn {
        const ast::FunctionDecl* decl = nullptr;
        std::vector<const ast::Call*> sites;
        std::vector<bool> proven;
    };
    std::vector<CandidateFn> candidateFns;

    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl == nullptr || fnDecl->isGenerator || fnDecl->isAsync) continue;
        if (fnDecl->name.empty() || fnDecl->params.empty()) continue;
        if (rebound.contains(fnDecl->name)) continue;
        bool shapeFits = true;
        for (const auto& p : fnDecl->params) {
            if (p.defaultValue || p.isRest || p.pattern || p.name.empty()) shapeFits = false;
        }
        if (!shapeFits) continue;

        CalleeOnlyScan scan(fnDecl->name);
        for (const auto& s : stmts) {
            if (s) s->accept(scan);
        }
        for (const auto& p : params) {
            if (p.defaultValue) p.defaultValue->accept(scan);
            if (p.pattern) ast::detail::visitPatternExprs(p.pattern.get(), scan);
        }
        if (scan.escaped || scan.spread || scan.sites.empty()) continue;

        candidateFns.push_back(CandidateFn{
            fnDecl, std::move(scan.sites), std::vector<bool>(fnDecl->params.size(), true)});
    }
    if (candidateFns.empty()) return;

    // Coupled greatest fixpoint over top-level lexical bindings in `stmts` and
    // candidate closure parameters, so a helper `add(dx)` that updates a
    // captured accumulator `total = total + dx` and is called as `add(total + 1)`
    // proves both `dx` and `total` numeric simultaneously.
    std::unordered_set<std::string> varCandidates;
    std::unordered_map<std::string, const ast::Expr*> varInits;
    std::unordered_set<const ast::VarDecl*> ownDecls;
    for (const auto& stmt : stmts) {
        const auto* vd = dynamic_cast<const ast::VarDecl*>(stmt.get());
        if (vd == nullptr || vd->pattern || vd->name.empty() || vd->isVar || vd->init == nullptr) {
            continue;
        }
        varCandidates.insert(vd->name);
        varInits[vd->name] = vd->init.get();
        ownDecls.insert(vd);
    }
    for (const auto& p : params) {
        if (!p.pattern && !p.name.empty()) {
            varCandidates.erase(p.name);
            varInits.erase(p.name);
        }
    }

    class CoupledWriteScan final : public ast::detail::IdentVisitor {
    public:
        CoupledWriteScan(const std::unordered_set<std::string>& vars,
                         const std::unordered_set<const ast::VarDecl*>& own)
            : vars_(vars), own_(own) {}
        std::unordered_set<std::string> disqualified;
        std::unordered_map<std::string, std::vector<std::pair<const ast::FunctionDecl*, const ast::Expr*>>> writes;
        const ast::FunctionDecl* currentFn = nullptr;

        void visit(const ast::VarDecl& v) override {
            if (own_.count(&v) == 0) {
                if (v.pattern) {
                    for (const auto& n : ast::patternBoundNames(*v.pattern)) disqualified.insert(n);
                } else {
                    disqualified.insert(v.name);
                }
            }
            ast::detail::IdentVisitor::visit(v);
        }
        void visit(const ast::FunctionDecl& f) override {
            disqualified.insert(f.name);
            for (const auto& p : f.params) {
                if (!p.pattern && !p.name.empty()) disqualified.insert(p.name);
            }
            const ast::FunctionDecl* saved = currentFn;
            currentFn = &f;
            ast::detail::IdentVisitor::visit(f);
            currentFn = saved;
        }
        void visit(const ast::FunctionExpr& f) override {
            disqualified.insert(f.name);
            for (const auto& p : f.params) {
                if (!p.pattern && !p.name.empty()) disqualified.insert(p.name);
            }
            ast::detail::IdentVisitor::visit(f);
        }
        void visit(const ast::Binary& b) override {
            if (ast::isAssignOp(b.op)) {
                if (const auto* target = dynamic_cast<const ast::Ident*>(b.lhs.get());
                    target != nullptr && vars_.count(target->name) != 0) {
                    writes[target->name].emplace_back(currentFn, b.rhs.get());
                }
            }
            ast::detail::IdentVisitor::visit(b);
        }
    private:
        const std::unordered_set<std::string>& vars_;
        const std::unordered_set<const ast::VarDecl*>& own_;
    };

    CoupledWriteScan writeScan(varCandidates, ownDecls);
    for (const auto& s : stmts) {
        if (s) s->accept(writeScan);
    }
    for (const auto& d : writeScan.disqualified) varCandidates.erase(d);

    std::function<bool(const ast::Expr&, const ast::FunctionDecl*, int)> numericUnder =
        [&](const ast::Expr& e, const ast::FunctionDecl* inFn, int depth) -> bool {
        if (depth <= 0) return false;
        if (provenNumber(e) || pristineMathCall(e)) return true;
        if (dynamic_cast<const ast::NumberLit*>(&e) != nullptr) return true;
        if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
            if (inFn != nullptr) {
                for (const auto& cf : candidateFns) {
                    if (cf.decl != inFn) continue;
                    for (size_t k = 0; k < inFn->params.size(); ++k) {
                        if (inFn->params[k].name == id->name) return cf.proven[k];
                    }
                }
            }
            return varCandidates.count(id->name) != 0;
        }
        if (const auto* un = dynamic_cast<const ast::Unary*>(&e)) {
            if (un->op == ast::UnaryOp::Posate) return true;
            if (un->op == ast::UnaryOp::Negate || un->op == ast::UnaryOp::BitNot ||
                un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
                un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) {
                return numericUnder(*un->operand, inFn, depth - 1);
            }
            return false;
        }
        if (const auto* bin = dynamic_cast<const ast::Binary*>(&e)) {
            if (ast::isAssignOp(bin->op)) return false;
            if (bin->op == ast::BinaryOp::Add || alwaysCoercingBinary(bin->op) ||
                bin->op == ast::BinaryOp::NullishCoalescing ||
                bin->op == ast::BinaryOp::LogicalOr) {
                return numericUnder(*bin->lhs, inFn, depth - 1) &&
                       numericUnder(*bin->rhs, inFn, depth - 1);
            }
            return false;
        }
        if (const auto* tern = dynamic_cast<const ast::Ternary*>(&e)) {
            return numericUnder(*tern->thenExpr, inFn, depth - 1) &&
                   numericUnder(*tern->elseExpr, inFn, depth - 1);
        }
        return false;
    };

    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = varCandidates.begin(); it != varCandidates.end();) {
            const auto initIt = varInits.find(*it);
            bool ok = (initIt == varInits.end()) || numericUnder(*initIt->second, nullptr, 8);
            if (ok) {
                if (const auto wIt = writeScan.writes.find(*it); wIt != writeScan.writes.end()) {
                    for (const auto& [fnCtx, rhs] : wIt->second) {
                        if (!numericUnder(*rhs, fnCtx, 8)) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            if (ok) {
                ++it;
            } else {
                it = varCandidates.erase(it);
                changed = true;
            }
        }
        for (auto& cf : candidateFns) {
            for (size_t k = 0; k < cf.proven.size(); ++k) {
                if (!cf.proven[k]) continue;
                for (const ast::Call* site : cf.sites) {
                    if (k >= site->args.size() || !numericUnder(*site->args[k], nullptr, 8)) {
                        cf.proven[k] = false;
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    for (auto& cf : candidateFns) {
        bool any = false;
        for (const bool p : cf.proven) any = any || p;
        if (any) provenClosureParams_.back().emplace(cf.decl, std::move(cf.proven));
    }
}

void Lowerer::applyProvenClosureParams(const ast::Node& site,
                                       const std::vector<ast::Param>& params,
                                       il::Function& fn) const {
    const auto* decl = dynamic_cast<const ast::FunctionDecl*>(&site);
    if (decl == nullptr) return;
    // The innermost open frame is the one planned for the statement list this
    // declaration is a member of: the enclosing body is mid-lowering, and every
    // body nested inside it has closed its own frame again. The module top level
    // is not lowered through `lowerFunctionBody` and so opens no frame at all.
    const size_t base = fn.firstSourceParam();
    if (!provenClosureParams_.empty()) {
        const auto& plan = provenClosureParams_.back();
        const auto it = plan.find(decl);
        if (it != plan.end()) {
            for (size_t i = 0;
                 i < params.size() && i < it->second.size() && i + base < fn.params.size(); ++i) {
                // Only ever Dynamic to F64. A `--pins` entry that already said the same
                // thing has run just above; the proof and the promise agree, and
                // neither can be reached by a slot the other typed differently.
                if (it->second[i] && fn.params[i + base].type == il::Type::Dynamic) {
                    fn.params[i + base].type = il::Type::F64;
                }
            }
        }
    }
    if (!fnParamFactsStack_.empty()) {
        const auto factsIt = fnParamFactsStack_.back().find(decl);
        if (factsIt != fnParamFactsStack_.back().end()) {
            for (size_t i = 0; i < params.size() && i + base < fn.params.size(); ++i) {
                if (fn.params[i + base].type == il::Type::F64 && !fn.params[i + base].pinned) {
                    const auto cIt = factsIt->second.constIntParams.find(params[i].name);
                    if (cIt != factsIt->second.constIntParams.end()) {
                        fn.params[i + base].knownConstInt = cIt->second;
                    }
                }
            }
        }
    }
}

}  // namespace bronze::lower
