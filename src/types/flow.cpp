#include "types/flow.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "ast/queries.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

// The ordered property names a constructor installs on `this`, in source
// order. Does not descend into nested functions: each one binds its own
// receiver, so an inner `this.x =` says nothing about this constructor
// (docs/0008 decision 3).
//
// Conditional assignments are collected unconditionally, so this can name a
// class the runtime never builds. That is deliberate and safe: decision 7
// keeps the shape guard even on a proven site, because the proof is over
// this compilation's source and the shape word is the runtime's authority.
class ThisPropertyWalker final : public Walker {
public:
    std::vector<std::string> properties;

    void visit(const ast::FunctionExpr&) override {}
    void visit(const ast::FunctionDecl&) override {}

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Assign) {
            if (const auto* member = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                if (dynamic_cast<const ast::ThisExpr*>(member->object.get())) {
                    if (std::find(properties.begin(), properties.end(), member->property) ==
                        properties.end()) {
                        properties.push_back(member->property);
                    }
                }
            }
        }
        Walker::visit(n);
    }
};

const char* statementLabel(const ast::Stmt& s) {
    if (const auto* v = dynamic_cast<const ast::VarDecl*>(&s)) {
        return v->isConst ? "const" : v->isVar ? "var" : "let";
    }
    if (dynamic_cast<const ast::ReturnStmt*>(&s)) return "return";
    if (dynamic_cast<const ast::ExprStmt*>(&s)) return "expr";
    if (dynamic_cast<const ast::IfStmt*>(&s)) return "if";
    if (dynamic_cast<const ast::WhileStmt*>(&s)) return "while";
    if (dynamic_cast<const ast::DoWhileStmt*>(&s)) return "do-while";
    if (dynamic_cast<const ast::ForStmt*>(&s)) return "for";
    if (dynamic_cast<const ast::BlockStmt*>(&s)) return "block";
    if (dynamic_cast<const ast::BreakStmt*>(&s)) return "break";
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) return "continue";
    if (dynamic_cast<const ast::SwitchStmt*>(&s)) return "switch";
    if (dynamic_cast<const ast::ForInStmt*>(&s)) return "for-in";
    if (dynamic_cast<const ast::ForOfStmt*>(&s)) return "for-of";
    if (dynamic_cast<const ast::TryStmt*>(&s)) return "try";
    if (dynamic_cast<const ast::ThrowStmt*>(&s)) return "throw";
    if (dynamic_cast<const ast::FunctionDecl*>(&s)) return "function";
    return nullptr;
}

// A body whose last statement is a `return` cannot fall off the end.
// Anything else might, and falling off the end yields `undefined`. The
// approximation only ever widens the return type, so it is sound.
bool bodyFallsThrough(const std::vector<const ast::Stmt*>& body) {
    if (body.empty()) return true;
    return dynamic_cast<const ast::ReturnStmt*>(body.back()) == nullptr;
}

// ⊥ in, ⊥ out: an operand no value has reached yet cannot produce one. This
// is what lets a recursive function read its own not-yet-known return type
// without the estimate jumping straight to `Dynamic` (decision 5).
Type withBottom(Type operand, Type result) {
    return operand.is(TypeKind::Never) ? Type::never() : result;
}
Type withBottom(Type a, Type b, Type result) {
    return (a.is(TypeKind::Never) || b.is(TypeKind::Never)) ? Type::never() : result;
}

// `-`, `*`, `/` and `%` are ToNumber on both operands, so the result is a
// number whatever came in (NaN is a number). `+` is concatenation as soon as
// either side is a string, because ToPrimitive on a string operand wins
// however the other side prints.
bool isNumericPrimitive(Type t) {
    switch (t.kind()) {
        case TypeKind::Number:
        case TypeKind::Bool:
        case TypeKind::Null:
        case TypeKind::Undefined: return true;
        default: return false;
    }
}

Type arithResult(ast::BinaryOp op, Type l, Type r) {
    if (l.is(TypeKind::Never) || r.is(TypeKind::Never)) return Type::never();
    if (op != ast::BinaryOp::Add) return Type::number();
    if (l.is(TypeKind::String) || r.is(TypeKind::String)) return Type::string();
    if (isNumericPrimitive(l) && isNumericPrimitive(r)) return Type::number();
    return Type::dynamic();
}

// The plain operator behind a compound assignment.
bool compoundOp(ast::BinaryOp op, ast::BinaryOp& out) {
    switch (op) {
        case ast::BinaryOp::PlusAssign: out = ast::BinaryOp::Add; return true;
        case ast::BinaryOp::MinusAssign: out = ast::BinaryOp::Sub; return true;
        case ast::BinaryOp::StarAssign: out = ast::BinaryOp::Mul; return true;
        case ast::BinaryOp::SlashAssign: out = ast::BinaryOp::Div; return true;
        case ast::BinaryOp::PercentAssign: out = ast::BinaryOp::Mod; return true;
        default: return false;
    }
}

struct LoopParts {
    const ast::Expr* condition = nullptr;  // null for `for (;;)`
    const std::vector<ast::StmtPtr>* body = nullptr;
    const ast::Expr* update = nullptr;
    bool conditionFirst = true;  // false for do-while
};

constexpr size_t kNoSlot = static_cast<size_t>(-1);

// Walks one function body in source order carrying `name -> Type`
// (decision 3). Constructed fresh per pass; the `Scope` it works on outlives
// it, because the env-backed cells have to survive the flow fixpoint.
class FlowAnalyzer final {
public:
    FlowAnalyzer(ModuleContext& mod, Scope& scope, FunctionFacts& facts,
                 std::string qualifiedName, bool record)
        : mod_(mod),
          scope_(scope),
          facts_(facts),
          qualifiedName_(std::move(qualifiedName)),
          record_(record) {}

    void runBody(const std::vector<const ast::Stmt*>& body) { stmtList(body, 0); }

    Type inferredReturn(const std::vector<const ast::Stmt*>& body) const {
        Type t = returnAccum_;
        if (bodyFallsThrough(body)) t = join(t, Type::undefined());
        return t;
    }

private:
    // ---- environment -------------------------------------------------------

    // Only the flow-sensitive bindings. The env-backed cells are reported
    // once for the whole function, since that is the granularity at which
    // they are true.
    const Env& visible() const { return scope_.env; }

    Type lookup(const std::string& name) const {
        if (const auto it = scope_.env.find(name); it != scope_.env.end()) return it->second;
        if (const auto it = scope_.cells.find(name); it != scope_.cells.end()) return it->second;
        for (const Scope* p = scope_.parent; p != nullptr; p = p->parent) {
            if (const auto it = p->cells.find(name); it != p->cells.end()) return it->second;
        }
        if (const auto it = mod_.indexByName.find(name); it != mod_.indexByName.end()) {
            return Type::function(it->second);
        }
        return Type::dynamic();
    }

    void declare(const std::string& name, Type t) {
        if (scope_.captured.count(name) != 0) {
            scope_.cells[name] = join(scope_.cells[name], t);
        } else {
            scope_.env[name] = t;
        }
    }

    // What a block-scoped statement list shadowed, so it can be put back.
    // Without this, `let x = 1; { let x = "s"; }` would leave the outer `x`
    // believing it is a string — an unsound narrowing, not a widening.
    //
    // `var` is deliberately absent: it is function-scoped wherever it is
    // written, which is exactly the line getScopeDeclarations draws.
    using ScopeSave = std::vector<std::pair<std::string, std::optional<Type>>>;

    ScopeSave saveDeclarations(const std::vector<std::string>& names) {
        ScopeSave saved;
        saved.reserve(names.size());
        for (const auto& name : names) {
            const auto it = scope_.env.find(name);
            saved.emplace_back(name, it == scope_.env.end() ? std::optional<Type>()
                                                            : std::optional<Type>(it->second));
        }
        return saved;
    }

    void restoreDeclarations(const ScopeSave& saved) {
        for (const auto& entry : saved) {
            if (entry.second.has_value()) {
                scope_.env[entry.first] = *entry.second;
            } else {
                scope_.env.erase(entry.first);
            }
        }
    }

    template <typename List>
    void scopedStmtList(const List& stmts, uint32_t depth) {
        const ScopeSave saved = saveDeclarations(ast::getScopeDeclarations(stmts));
        stmtList(stmts, depth);
        restoreDeclarations(saved);
    }

    void assign(const std::string& name, Type t) {
        if (scope_.captured.count(name) != 0) {
            scope_.cells[name] = join(scope_.cells[name], t);
            return;
        }
        if (const auto it = scope_.env.find(name); it != scope_.env.end()) {
            it->second = t;
            return;
        }
        // Not ours. A closure can only reach an enclosing function's
        // env-backed cells, so that is the only other thing this can be;
        // anything else is a global, which nothing here tracks.
        for (Scope* p = scope_.parent; p != nullptr; p = p->parent) {
            if (const auto it = p->cells.find(name); it != p->cells.end()) {
                it->second = join(it->second, t);
                return;
            }
        }
    }

    // A statement whose effects the AST does not expose invalidates every
    // binding. Widening to Dynamic is sound and is the designed fallback,
    // not a silent lie about semantics.
    void widenAll() {
        for (auto& entry : scope_.env) entry.second = Type::dynamic();
        for (auto& entry : scope_.cells) entry.second = Type::dynamic();
    }

    // ---- dump recording ----------------------------------------------------

    size_t pushStmt(const char* label, uint32_t index, uint32_t depth) {
        if (!record_) return kNoSlot;
        StatementFacts sf;
        sf.depth = depth;
        sf.index = index;
        sf.label = label;
        facts_.statements.push_back(std::move(sf));
        return facts_.statements.size() - 1;
    }

    void pushMarker(const char* label, uint32_t depth) {
        if (!record_) return;
        StatementFacts sf;
        sf.depth = depth;
        sf.isMarker = true;
        sf.label = label;
        facts_.statements.push_back(std::move(sf));
    }

    void closeStmt(size_t slot, const Env& before) {
        if (slot == kNoSlot) return;
        const Env after = visible();
        std::vector<BindingChange> changes;
        for (const auto& entry : after) {
            const auto it = before.find(entry.first);
            if (it == before.end() || it->second != entry.second) {
                changes.push_back(BindingChange{entry.first, entry.second});
            }
        }
        // `after` is an ordered map, so `changes` is already sorted by name.
        facts_.statements[slot].changes = std::move(changes);
    }

    // The join a merge point produced, keyed by the statement that owns it.
    // Recorded only on the final walk, like every other side-table entry, so
    // a probe pass's half-converged environment can never be what lowering
    // reads (`InferenceResult::typeOfBindingAt` is the contract).
    void recordMerge(const ast::Stmt& mergePoint, const Env& env) {
        if (!record_) return;
        mod_.result->mergeBindings[&mergePoint] = env;
    }

    void fail(Span span, const std::string& what) {
        if (mod_.failed) return;
        mod_.failed = true;
        mod_.diags->error(span, "internal: type inference " + what);
    }

    // ---- statements --------------------------------------------------------

    void stmtList(const std::vector<ast::StmtPtr>& stmts, uint32_t depth) {
        uint32_t index = 0;
        for (const auto& s : stmts) {
            if (s) stmt(*s, index++, depth);
        }
    }
    void stmtList(const std::vector<const ast::Stmt*>& stmts, uint32_t depth) {
        uint32_t index = 0;
        for (const auto* s : stmts) {
            if (s) stmt(*s, index++, depth);
        }
    }

    void stmt(const ast::Stmt& s, uint32_t index, uint32_t depth) {
        const char* label = statementLabel(s);
        if (label == nullptr) {
            fail(s.span, "saw an unknown statement node kind");
            return;
        }
        const Env before = visible();
        const size_t slot = pushStmt(label, index, depth);
        dispatch(s, depth);
        closeStmt(slot, before);
    }

    void dispatch(const ast::Stmt& s, uint32_t depth) {
        if (const auto* v = dynamic_cast<const ast::VarDecl*>(&s)) {
            // A `let` with no initialiser holds `undefined` at this point,
            // not "number or undefined": the flow analysis is what turns a
            // later single assignment into a precise type, which is why the
            // lattice does not need a union (decision 2).
            //
            // Annotations are deliberately not consulted. Seeding from one
            // would let it widen what bronze believes, and decision 6 makes
            // an annotation something that can only agree with a proof or be
            // discarded. Landing that (with its warnings) is step 5.
            declare(v->name, v->init ? expr(*v->init) : Type::undefined());
            return;
        }
        if (const auto* r = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            returnAccum_ = join(returnAccum_, r->value ? expr(*r->value) : Type::undefined());
            return;
        }
        if (const auto* e = dynamic_cast<const ast::ExprStmt*>(&s)) {
            expr(*e->expr);
            return;
        }
        if (const auto* b = dynamic_cast<const ast::BlockStmt*>(&s)) {
            scopedStmtList(b->stmts, depth + 1);
            return;
        }
        if (const auto* i = dynamic_cast<const ast::IfStmt*>(&s)) {
            ifStmt(*i, depth);
            return;
        }
        if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) {
            LoopParts parts;
            parts.condition = w->condition.get();
            parts.body = &w->body;
            analyzeLoop(parts, depth, s);
            return;
        }
        if (const auto* d = dynamic_cast<const ast::DoWhileStmt*>(&s)) {
            LoopParts parts;
            parts.condition = d->condition.get();
            parts.body = &d->body;
            parts.conditionFirst = false;
            analyzeLoop(parts, depth, s);
            return;
        }
        if (const auto* f = dynamic_cast<const ast::ForStmt*>(&s)) {
            // `for (let i = ...)` binds `i` in the loop's own scope, which
            // spans the condition, the body and the update and ends here.
            std::vector<const ast::Stmt*> initList;
            if (f->init) initList.push_back(f->init.get());
            const ScopeSave saved = saveDeclarations(ast::getScopeDeclarations(initList));
            if (f->init) {
                pushMarker("init", depth + 1);
                stmt(*f->init, 0, depth + 2);
            }
            LoopParts parts;
            parts.condition = f->condition.get();
            parts.body = &f->body;
            parts.update = f->update.get();
            analyzeLoop(parts, depth, s);
            restoreDeclarations(saved);
            return;
        }
        if (dynamic_cast<const ast::BreakStmt*>(&s)) {
            if (!breakStack_.empty()) breakStack_.back().push_back(scope_.env);
            return;
        }
        if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
            if (!continueStack_.empty()) continueStack_.back().push_back(scope_.env);
            return;
        }
        if (const auto* sw = dynamic_cast<const ast::SwitchStmt*>(&s)) {
            if (sw->discriminant) expr(*sw->discriminant);
            widenAll();
            return;
        }
        if (const auto* fd = dynamic_cast<const ast::FunctionDecl*>(&s)) {
            // A nested declaration is a closure value (docs/0007 decision 4),
            // so it carries no module function index and no direct call.
            declare(fd->name, Type::function());
            analyzeNested(*fd, fd->name, fd->params, fd->body, fd->span);
            return;
        }
        // ForIn / ForOf / Try / Throw are parsed as childless nodes; nothing
        // under them is visible, so nothing can be believed across them.
        widenAll();
    }

    void ifStmt(const ast::IfStmt& i, uint32_t depth) {
        expr(*i.condition);
        const Env entry = scope_.env;
        if (!i.thenBody.empty()) {
            pushMarker("then", depth + 1);
            scopedStmtList(i.thenBody, depth + 2);
        }
        const Env thenEnv = scope_.env;
        scope_.env = entry;
        if (!i.elseBody.empty()) {
            pushMarker("else", depth + 1);
            scopedStmtList(i.elseBody, depth + 2);
        }
        scope_.env = joinEnv(thenEnv, scope_.env);
        recordMerge(i, scope_.env);
    }

    // One structured loop. The header environment is the join of the loop's
    // entry, the end of the body, and every `continue`; iterate until it
    // stops moving. Probe iterations never record, so the dump gets the body
    // exactly once, from a final run over the converged header.
    void analyzeLoop(const LoopParts& parts, uint32_t depth, const ast::Stmt& stmt) {
        const Span span = stmt.span;
        Env header = scope_.env;
        bool converged = false;
        for (uint32_t iter = 0; iter <= kMaxFlowIterations; ++iter) {
            const Env beforeHeader = header;
            const Env beforeCells = scope_.cells;
            runLoopOnce(parts, header, depth, /*record=*/false);
            header = joinEnv(header, scope_.env);
            for (const Env& e : loopContinues_) header = joinEnv(header, e);
            if (header == beforeHeader && scope_.cells == beforeCells) {
                converged = true;
                break;
            }
        }
        if (!converged) {
            fail(span, "loop binding types did not converge");
            return;
        }

        runLoopOnce(parts, header, depth, record_);
        // The converged header already subsumes the end of the body, so it
        // is also the environment on the loop's normal exit; a `break` can
        // leave from anywhere inside, so those merge in separately.
        Env exit = header;
        for (const Env& e : loopBreaks_) exit = joinEnv(exit, e);
        // One recorded answer for every merge this loop builds. `exit`
        // subsumes the converged header (it is that header joined with the
        // `break` environments), and the header in turn subsumes the entry
        // edge, the end of the body and every `continue` — so this is an
        // upper bound of every edge into the header, the update/condition
        // block and the exit block alike. Lowering needs exactly that: it
        // types the header's parameters before it has lowered the back edge.
        recordMerge(stmt, exit);
        scope_.env = std::move(exit);
    }

    void runLoopOnce(const LoopParts& parts, const Env& header, uint32_t depth, bool record) {
        scope_.env = header;
        breakStack_.emplace_back();
        continueStack_.emplace_back();
        const bool saved = record_;
        record_ = record;
        if (parts.conditionFirst && parts.condition != nullptr) expr(*parts.condition);
        if (parts.body != nullptr && !parts.body->empty()) {
            pushMarker("body", depth + 1);
            scopedStmtList(*parts.body, depth + 2);
        }
        if (!parts.conditionFirst && parts.condition != nullptr) expr(*parts.condition);
        if (parts.update != nullptr) expr(*parts.update);
        record_ = saved;
        loopBreaks_ = std::move(breakStack_.back());
        breakStack_.pop_back();
        loopContinues_ = std::move(continueStack_.back());
        continueStack_.pop_back();
    }

    // ---- expressions -------------------------------------------------------

    Type expr(const ast::Expr& e) {
        const Type t = exprKind(e);
        if (record_) mod_.result->exprTypes[&e] = t;
        return t;
    }

    Type exprKind(const ast::Expr& e) {
        if (dynamic_cast<const ast::NumberLit*>(&e)) return Type::number();
        if (dynamic_cast<const ast::StringLit*>(&e)) return Type::string();
        if (dynamic_cast<const ast::BoolLit*>(&e)) return Type::boolean();
        if (dynamic_cast<const ast::NullLit*>(&e)) return Type::null();
        if (dynamic_cast<const ast::UndefinedLit*>(&e)) return Type::undefined();
        // `this` is the caller's receiver; nothing here proves anything about
        // it. Sharpening it needs the prototype work of decision 4 applied to
        // constructors, which is the property-access step.
        if (dynamic_cast<const ast::ThisExpr*>(&e)) return Type::dynamic();
        if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) return lookup(id->name);
        if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) return unary(*u);
        if (const auto* b = dynamic_cast<const ast::Binary*>(&e)) return binary(*b);
        if (const auto* t = dynamic_cast<const ast::Ternary*>(&e)) {
            expr(*t->condition);
            const Env entry = scope_.env;
            const Type a = expr(*t->thenExpr);
            const Env thenEnv = scope_.env;
            scope_.env = entry;
            const Type b = expr(*t->elseExpr);
            scope_.env = joinEnv(thenEnv, scope_.env);
            return join(a, b);
        }
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&e)) {
            expr(*m->object);
            // v1 proves the receiver's shape class, never the property's
            // type; that is what decision 7 consumes and all it needs.
            return Type::dynamic();
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&e)) {
            expr(*ix->object);
            expr(*ix->index);
            return Type::dynamic();
        }
        if (const auto* c = dynamic_cast<const ast::Call*>(&e)) return call(*c);
        if (const auto* n = dynamic_cast<const ast::NewExpr*>(&e)) return newExpr(*n);
        if (const auto* o = dynamic_cast<const ast::ObjectLit*>(&e)) return objectLit(*o);
        if (const auto* a = dynamic_cast<const ast::ArrayLit*>(&e)) {
            for (const auto& el : a->elements) expr(*el);
            // An array is an object with no property-name identity; there is
            // no shape class to prove about it.
            return Type::object();
        }
        if (const auto* f = dynamic_cast<const ast::FunctionExpr*>(&e)) {
            analyzeNested(*f, f->name, f->params, f->body, f->span);
            return Type::function();
        }
        fail(e.span, "saw an unknown expression node kind");
        return Type::dynamic();
    }

    Type unary(const ast::Unary& u) {
        const Type operand = expr(*u.operand);
        switch (u.op) {
            case ast::UnaryOp::Not: return withBottom(operand, Type::boolean());
            case ast::UnaryOp::Negate:
            case ast::UnaryOp::Posate: return withBottom(operand, Type::number());
            case ast::UnaryOp::PreInc:
            case ast::UnaryOp::PreDec:
            case ast::UnaryOp::PostInc:
            case ast::UnaryOp::PostDec:
                // ToNumber, so the binding holds a number afterwards whatever
                // it held before. The one place a use site sharpens a name.
                if (const auto* id = dynamic_cast<const ast::Ident*>(u.operand.get())) {
                    assign(id->name, Type::number());
                }
                return Type::number();
        }
        return Type::dynamic();
    }

    Type binary(const ast::Binary& b) {
        if (b.op == ast::BinaryOp::Assign) {
            const Type rhs = expr(*b.rhs);
            if (const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get())) {
                assign(id->name, rhs);
            } else {
                expr(*b.lhs);
            }
            return rhs;
        }
        ast::BinaryOp plain{};
        if (compoundOp(b.op, plain)) {
            const Type rhs = expr(*b.rhs);
            const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get());
            const Type current = id != nullptr ? lookup(id->name) : expr(*b.lhs);
            const Type result = arithResult(plain, current, rhs);
            if (id != nullptr) assign(id->name, result);
            return result;
        }

        // `&&` / `||` / `??` are short-circuiting, so the right operand may
        // not run; joining its environment back in would be a claim it did.
        // The value is one operand or the other, hence the type join.
        if (b.op == ast::BinaryOp::LogicalAnd || b.op == ast::BinaryOp::LogicalOr ||
            b.op == ast::BinaryOp::NullishCoalescing) {
            const Type l = expr(*b.lhs);
            const Env entry = scope_.env;
            const Type r = expr(*b.rhs);
            scope_.env = joinEnv(entry, scope_.env);
            return join(l, r);
        }

        const Type l = expr(*b.lhs);
        const Type r = expr(*b.rhs);
        switch (b.op) {
            case ast::BinaryOp::Add:
            case ast::BinaryOp::Sub:
            case ast::BinaryOp::Mul:
            case ast::BinaryOp::Div:
            case ast::BinaryOp::Mod: return arithResult(b.op, l, r);
            case ast::BinaryOp::Less:
            case ast::BinaryOp::Greater:
            case ast::BinaryOp::LessEqual:
            case ast::BinaryOp::GreaterEqual:
            case ast::BinaryOp::Eq:
            case ast::BinaryOp::StrictEq:
            case ast::BinaryOp::Ne:
            case ast::BinaryOp::StrictNe: return withBottom(l, r, Type::boolean());
            default: break;
        }
        return Type::dynamic();
    }

    Type call(const ast::Call& c) {
        const Type calleeType = expr(*c.callee);
        std::vector<Type> args;
        args.reserve(c.args.size());
        for (const auto& a : c.args) args.push_back(expr(*a));

        const uint32_t index = calleeType.functionIndex();
        if (index == kNoFunctionIndex) return Type::dynamic();

        FunctionInfo& callee = mod_.functions[index];
        if (!callee.directCallable) return Type::dynamic();

        // This site's contribution to the callee's parameters. A missing
        // argument is `undefined`, exactly as the call would deliver it.
        for (size_t i = 0; i < callee.observedParams.size(); ++i) {
            const Type at = i < args.size() ? args[i] : Type::undefined();
            callee.observedParams[i] = join(callee.observedParams[i], at);
        }
        return callee.signature.returnType;
    }

    Type newExpr(const ast::NewExpr& n) {
        for (const auto& a : n.args) expr(*a);
        const ShapeClassId cls = constructorShape(n.callee);
        if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
        return Type::object(cls);
    }

    ShapeClassId constructorShape(const std::string& name) {
        const auto found = mod_.indexByName.find(name);
        if (found == mod_.indexByName.end()) return kNoShapeClass;
        const uint32_t index = found->second;
        if (const auto it = mod_.ctorShapes.find(index); it != mod_.ctorShapes.end()) {
            return it->second;
        }
        ThisPropertyWalker walker;
        walker.walkList(mod_.functions[index].decl->body);
        const ShapeClassId cls = mod_.result->shapes.intern(name, std::move(walker.properties));
        mod_.ctorShapes.emplace(index, cls);
        return cls;
    }

    Type objectLit(const ast::ObjectLit& o) {
        std::vector<std::string> props;
        for (const auto& p : o.props) {
            expr(*p.value);
            // A duplicate key overwrites; it does not transition again.
            if (std::find(props.begin(), props.end(), p.key) == props.end()) {
                props.push_back(p.key);
            }
        }
        // Empty constructor name: a plain literal's prototype is the one root
        // shape every `{}` shares (docs/0008 decision 1).
        const ShapeClassId cls = mod_.result->shapes.intern(std::string(), std::move(props));
        if (record_) mod_.result->siteShapes[&o] = cls;
        return Type::object(cls);
    }

    void analyzeNested(const ast::Node& site, const std::string& declaredName,
                       const std::vector<ast::Param>& params,
                       const std::vector<ast::StmtPtr>& body, Span span) {
        std::string name = declaredName;
        if (name.empty()) name = "<anon" + std::to_string(anonCounter_++) + ">";
        std::vector<const ast::Stmt*> borrowed;
        borrowed.reserve(body.size());
        for (const auto& s : body) borrowed.push_back(s.get());

        // A closure is never a direct-call target (docs/0007), so its
        // parameters keep the uniform dynamic convention.
        const std::vector<Type> paramTypes(params.size(), Type::dynamic());
        analyzeFunction(mod_, &scope_, qualifiedName_ + "::" + name, kNoFunctionIndex, &site,
                        /*directCallable=*/false, params, paramTypes, borrowed, span, record_);
    }

    ModuleContext& mod_;
    Scope& scope_;
    FunctionFacts& facts_;
    std::string qualifiedName_;
    bool record_ = false;

    Type returnAccum_ = Type::never();
    uint32_t anonCounter_ = 0;
    std::vector<std::vector<Env>> breakStack_;
    std::vector<std::vector<Env>> continueStack_;
    std::vector<Env> loopBreaks_;
    std::vector<Env> loopContinues_;
};

void seedParams(Scope& scope, const std::vector<ast::Param>& params,
                const std::vector<Type>& paramTypes) {
    for (size_t i = 0; i < params.size(); ++i) {
        const Type t = i < paramTypes.size() ? paramTypes[i] : Type::dynamic();
        if (scope.captured.count(params[i].name) != 0) {
            scope.cells[params[i].name] = join(scope.cells[params[i].name], t);
        } else {
            scope.env[params[i].name] = t;
        }
    }
}

}  // namespace

Env joinEnv(const Env& a, const Env& b) {
    Env out;
    for (const auto& entry : a) {
        const auto it = b.find(entry.first);
        // A name in only one side is a block-scoped declaration that did not
        // survive the merge; dropping it makes a later read answer Dynamic,
        // which is the sound direction.
        if (it != b.end()) out.emplace(entry.first, join(entry.second, it->second));
    }
    return out;
}

FunctionOutcome analyzeFunction(ModuleContext& mod, Scope* parent,
                                const std::string& qualifiedName, uint32_t moduleIndex,
                                const ast::Node* site, bool directCallable,
                                const std::vector<ast::Param>& params,
                                const std::vector<Type>& paramTypes,
                                const std::vector<const ast::Stmt*>& body, Span span,
                                bool record) {
    Scope scope;
    scope.parent = parent;
    const auto captured = ast::getCapturedNames(body);
    scope.captured.insert(captured.begin(), captured.end());

    FunctionFacts facts;
    facts.name = qualifiedName;
    facts.index = moduleIndex;
    facts.directCallable = directCallable;
    facts.paramNames.reserve(params.size());
    for (const auto& p : params) facts.paramNames.push_back(p.name);
    facts.signature.params = paramTypes;

    // The env-backed cells are one value per name over the whole function, so
    // a write late in the body has to be visible to a read early in it. That
    // is a fixpoint, not a forward walk — run the body until the cells stop
    // moving, then once more for real.
    bool converged = false;
    for (uint32_t iter = 0; iter <= kMaxFlowIterations; ++iter) {
        const Env beforeCells = scope.cells;
        scope.env.clear();
        seedParams(scope, params, paramTypes);
        FlowAnalyzer probe(mod, scope, facts, qualifiedName, /*record=*/false);
        probe.runBody(body);
        if (mod.failed) return FunctionOutcome{Type::dynamic(), false};
        if (scope.cells == beforeCells) {
            converged = true;
            break;
        }
    }
    if (!converged) {
        mod.failed = true;
        mod.diags->error(span, "internal: type inference captured-variable types did not "
                               "converge in '" + qualifiedName + "'");
        return FunctionOutcome{Type::dynamic(), false};
    }

    // The slot is reserved before the final walk so that functions nested
    // inside this one, which append as they are discovered, land after it.
    size_t slot = kNoSlot;
    if (record) {
        mod.result->functions.emplace_back();
        slot = mod.result->functions.size() - 1;
    }

    scope.env.clear();
    seedParams(scope, params, paramTypes);
    FlowAnalyzer recorder(mod, scope, facts, qualifiedName, record);
    recorder.runBody(body);
    if (mod.failed) return FunctionOutcome{Type::dynamic(), false};

    const Type returnType = recorder.inferredReturn(body);
    facts.signature.returnType = returnType;
    // A closure's only queryable proof (docs/0010 decision 5 excludes it
    // from signature specialization, so its parameters stay dynamic � but
    // what its body returns is a fact about the body alone, and throwing it
    // away is what made every annotation on a closure unprovable).
    if (record && site != nullptr) mod.result->closureReturns[site] = returnType;
    // Ordered map in, sorted vector out.
    for (const auto& cell : scope.cells) {
        facts.cells.push_back(BindingChange{cell.first, cell.second});
    }
    if (slot != kNoSlot) mod.result->functions[slot] = std::move(facts);
    return FunctionOutcome{returnType, true};
}

}  // namespace bronze::types
