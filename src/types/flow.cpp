// The statement half of flow analysis: the environment a statement list
// carries, what survives a merge, the loop fixpoint, and the driver that runs
// a function body to convergence. The expression type rules are the other
// half and live in flow_expr.cpp; the seam is argued in flow_analyzer.h.

#include "types/flow.h"

#include <optional>
#include <utility>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "types/flow_analyzer.h"

namespace bronze::types {
namespace {

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
    if (dynamic_cast<const ast::LabeledStmt*>(&s)) return "label";
    if (dynamic_cast<const ast::TryStmt*>(&s)) return "try";
    if (dynamic_cast<const ast::ThrowStmt*>(&s)) return "throw";
    if (dynamic_cast<const ast::FunctionDecl*>(&s)) return "function";
    if (dynamic_cast<const ast::ClassDecl*>(&s)) return "class";
    return nullptr;
}

// A body whose last statement is a `return` cannot fall off the end.
// Anything else might, and falling off the end yields `undefined`. The
// approximation only ever widens the return type, so it is sound.
bool bodyFallsThrough(const std::vector<const ast::Stmt*>& body) {
    if (body.empty()) return true;
    return dynamic_cast<const ast::ReturnStmt*>(body.back()) == nullptr;
}

}  // namespace

// A parameter's default is CODE, evaluated in this function's scope on
// the calls that omit the argument. Skipping it would hide every call
// site inside it from the pass that widens callee signatures — the exact
// shape of an unsound proof (docs/0017 decision 9).
void FlowAnalyzer::runParamDefaults(const std::vector<ast::Param>& params) {
    for (const auto& param : params) {
        if (param.defaultValue) expr(*param.defaultValue);
        if (param.pattern) patternDefaults(*param.pattern);
    }
}

Type FlowAnalyzer::inferredReturn(const std::vector<const ast::Stmt*>& body) const {
    Type t = returnAccum_;
    if (bodyFallsThrough(body)) t = join(t, Type::undefined());
    return t;
}

// ---- environment -------------------------------------------------------

Type FlowAnalyzer::lookup(const std::string& name) const {
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

void FlowAnalyzer::declare(const std::string& name, Type t) {
    if (scope_.captured.count(name) != 0) {
        scope_.cells[name] = join(scope_.cells[name], t);
    } else {
        scope_.env[name] = t;
    }
}

void FlowAnalyzer::patternDefaults(const ast::BindingPattern& pattern) {
    for (const auto& elem : pattern.elements) {
        if (elem.keyExpr) expr(*elem.keyExpr);
        if (elem.defaultValue) expr(*elem.defaultValue);
        if (elem.pattern) patternDefaults(*elem.pattern);
    }
}

void FlowAnalyzer::declarePattern(const ast::BindingPattern& pattern) {
    patternDefaults(pattern);
    for (const auto& name : ast::patternBoundNames(pattern)) {
        declare(name, Type::dynamic());
    }
}

FlowAnalyzer::ScopeSave FlowAnalyzer::saveDeclarations(const std::vector<std::string>& names) {
    ScopeSave saved;
    saved.reserve(names.size());
    for (const auto& name : names) {
        const auto it = scope_.env.find(name);
        saved.emplace_back(name, it == scope_.env.end() ? std::optional<Type>()
                                                        : std::optional<Type>(it->second));
    }
    return saved;
}

void FlowAnalyzer::restoreDeclarations(const ScopeSave& saved) {
    for (const auto& entry : saved) {
        if (entry.second.has_value()) {
            scope_.env[entry.first] = *entry.second;
        } else {
            scope_.env.erase(entry.first);
        }
    }
}

void FlowAnalyzer::assign(const std::string& name, Type t) {
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

void FlowAnalyzer::widenAll() {
    for (auto& entry : scope_.env) entry.second = Type::dynamic();
    for (auto& entry : scope_.cells) entry.second = Type::dynamic();
}

// ---- dump recording ----------------------------------------------------

size_t FlowAnalyzer::pushStmt(const char* label, uint32_t index, uint32_t depth) {
    if (!record_) return kNoSlot;
    StatementFacts sf;
    sf.depth = depth;
    sf.index = index;
    sf.label = label;
    facts_.statements.push_back(std::move(sf));
    return facts_.statements.size() - 1;
}

void FlowAnalyzer::pushMarker(const char* label, uint32_t depth) {
    if (!record_) return;
    StatementFacts sf;
    sf.depth = depth;
    sf.isMarker = true;
    sf.label = label;
    facts_.statements.push_back(std::move(sf));
}

void FlowAnalyzer::closeStmt(size_t slot, const Env& before) {
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

void FlowAnalyzer::recordMerge(const ast::Stmt& mergePoint, const Env& env) {
    if (!record_) return;
    mod_.result->mergeBindings[&mergePoint] = env;
}

void FlowAnalyzer::fail(Span span, const std::string& what) {
    if (mod_.failed) return;
    mod_.failed = true;
    mod_.diags->error(span, "internal: type inference " + what);
}

// ---- statements --------------------------------------------------------

void FlowAnalyzer::stmtList(const std::vector<ast::StmtPtr>& stmts, uint32_t depth) {
    uint32_t index = 0;
    for (const auto& s : stmts) {
        if (s) stmt(*s, index++, depth);
    }
}
void FlowAnalyzer::stmtList(const std::vector<const ast::Stmt*>& stmts, uint32_t depth) {
    uint32_t index = 0;
    for (const auto* s : stmts) {
        if (s) stmt(*s, index++, depth);
    }
}

void FlowAnalyzer::stmt(const ast::Stmt& s, uint32_t index, uint32_t depth) {
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

void FlowAnalyzer::dispatch(const ast::Stmt& s, uint32_t depth) {
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
        // A destructuring declaration binds names whose values come out
        // of a read this pass does not model, so each is dynamic; the
        // initialiser is still walked, for its effects on call sites.
        if (v->pattern) {
            expr(*v->init);
            declarePattern(*v->pattern);
            return;
        }
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
        for (const auto& initStmt : f->init) initList.push_back(initStmt.get());
        const ScopeSave saved = saveDeclarations(ast::getScopeDeclarations(initList));
        if (!f->init.empty()) {
            pushMarker("init", depth + 1);
            for (const auto& initStmt : f->init) stmt(*initStmt, 0, depth + 2);
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
        switchStmt(*sw, depth);
        return;
    }
    if (const auto* tr = dynamic_cast<const ast::TryStmt*>(&s)) {
        tryStmt(*tr, depth);
        return;
    }
    if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (th->value) expr(*th->value);
        return;
    }
    if (const auto* lb = dynamic_cast<const ast::LabeledStmt*>(&s)) {
        // A label changes where a jump inside the statement goes, and
        // nothing about the types the statement produces.
        if (lb->body) stmt(*lb->body, 0, depth + 1);
        return;
    }
    if (const auto* fo = dynamic_cast<const ast::ForOfStmt*>(&s)) {
        keyedLoop(s, fo->iterable.get(), fo->name, fo->pattern.get(), fo->body, depth);
        return;
    }
    if (const auto* fi = dynamic_cast<const ast::ForInStmt*>(&s)) {
        keyedLoop(s, fi->object.get(), fi->name, fi->pattern.get(), fi->body, depth);
        return;
    }
    if (const auto* cd = dynamic_cast<const ast::ClassDecl*>(&s)) {
        // A class is a constructor function value, and each of its
        // methods is a closure (docs/0012 decision 5) — the same two
        // facts the branch below states about a nested declaration.
        declare(cd->name, Type::function());
        for (const auto& m : cd->methods) {
            analyzeNested(*m.fn, m.fn->name, m.fn->params, m.fn->body, m.fn->span);
        }
        return;
    }
    if (const auto* fd = dynamic_cast<const ast::FunctionDecl*>(&s)) {
        // A nested declaration is a closure value (docs/0007 decision 4),
        // so it carries no module function index and no direct call.
        declare(fd->name, Type::function());
        analyzeNested(*fd, fd->name, fd->params, fd->body, fd->span);
        return;
    }
    // Every statement kind `statementLabel` names has a case above, so this
    // is only reached by one added to the AST and not to this dispatch.
    // Widening is the sound answer to a construct whose effects are unknown,
    // and `stmt` has already refused anything `statementLabel` does not know.
    widenAll();
}

// for-of and for-in: one loop each, whose head binds names this pass
// proves nothing about — the element comes out of an indexed read and the
// key out of an enumeration, and neither has a type here.
//
// Walking the BODY is not an optimization. A call written only inside one
// of these loops was invisible to the widening pass, so a callee could be
// proven `number` while a string reached it from the loop — an unsound
// proof of exactly the shape docs/0017 decision 9 names.
void FlowAnalyzer::keyedLoop(const ast::Stmt& s, const ast::Expr* source, const std::string& name,
               const ast::BindingPattern* pattern, const std::vector<ast::StmtPtr>& body,
               uint32_t depth) {
    if (source) expr(*source);
    std::vector<std::string> headNames =
        pattern ? ast::patternBoundNames(*pattern) : std::vector<std::string>{name};
    const ScopeSave saved = saveDeclarations(headNames);
    if (pattern) {
        declarePattern(*pattern);
    } else if (!name.empty()) {
        declare(name, Type::dynamic());
    }
    LoopParts parts;
    parts.body = &body;
    analyzeLoop(parts, depth, s);
    restoreDeclarations(saved);
}

// A switch is many paths through one statement list, and this pass models
// no path splitting below a statement. So every case is walked for its
// effects — the case expressions are ordinary code, and so are the bodies
// — and the environment that survives is the entry one, widened. Sound
// because widening only ever loses precision, and it is what makes the
// call sites inside a case visible to the signature fixpoint.
void FlowAnalyzer::switchStmt(const ast::SwitchStmt& sw, uint32_t depth) {
    if (sw.discriminant) expr(*sw.discriminant);
    // A switch IS a breakable statement (ECMA-262 14.12), so an
    // unlabelled `break` inside one targets it and not an enclosing loop.
    breakStack_.emplace_back();
    const Env entry = scope_.env;
    for (const auto& c : sw.cases) {
        if (c.test) expr(*c.test);
        if (c.body.empty()) continue;
        pushMarker(c.test ? "case" : "default", depth + 1);
        scopedStmtList(c.body, depth + 2);
    }
    breakStack_.pop_back();
    scope_.env = entry;
    widenAll();
    recordMerge(sw, scope_.env);
}

// A try statement is three paths through one construct, and — like a
// switch — this pass models no path splitting below a statement. The
// catch clause is entered from an unknown point in the try block, so
// nothing the try block established survives into it, and nothing either
// of them established survives the statement.
//
// Walking all three matters for more than precision: a call written only
// inside a `try` has to be visible to the call-graph signature fixpoint,
// and an invisible call site is an unsound proof (docs/0018's second
// bug). Every binding these bodies assign is a CELL by decision 4 of
// docs/0020, so the widening below is belt-and-braces over a set that is
// normally empty.
void FlowAnalyzer::tryStmt(const ast::TryStmt& t, uint32_t depth) {
    pushMarker("block", depth + 1);
    scopedStmtList(t.body, depth + 2);
    if (t.hasCatch) {
        const ScopeSave saved =
            saveDeclarations(t.catchPattern
                                 ? ast::patternBoundNames(*t.catchPattern)
                                 : std::vector<std::string>{t.catchName});
        if (t.hasCatchParam) {
            if (t.catchPattern) {
                declarePattern(*t.catchPattern);
            } else {
                // Any value can be thrown, so a catch parameter is the one
                // binding whose type is dynamic by definition rather than
                // for want of a proof.
                declare(t.catchName, Type::dynamic());
            }
        }
        pushMarker("catch", depth + 1);
        scopedStmtList(t.catchBody, depth + 2);
        restoreDeclarations(saved);
    }
    if (t.hasFinally) {
        pushMarker("finally", depth + 1);
        scopedStmtList(t.finallyBody, depth + 2);
    }
    widenAll();
    recordMerge(t, scope_.env);
}

void FlowAnalyzer::ifStmt(const ast::IfStmt& i, uint32_t depth) {
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
void FlowAnalyzer::analyzeLoop(const LoopParts& parts, uint32_t depth, const ast::Stmt& stmt) {
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

void FlowAnalyzer::runLoopOnce(const LoopParts& parts, const Env& header, uint32_t depth, bool record) {
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

namespace {

void seedParams(Scope& scope, const std::vector<ast::Param>& params,
                const std::vector<Type>& paramTypes) {
    auto seedOne = [&](const std::string& name, Type t) {
        if (scope.captured.count(name) != 0) {
            scope.cells[name] = join(scope.cells[name], t);
        } else {
            scope.env[name] = t;
        }
    };
    for (size_t i = 0; i < params.size(); ++i) {
        const Type t = i < paramTypes.size() ? paramTypes[i] : Type::dynamic();
        // A pattern parameter has no name of its own, and nothing is known
        // about the names it does bind: they come out of a dynamic read of a
        // value whose shape this pass does not track (docs/0017 decision 5).
        if (params[i].pattern) {
            for (const auto& bound : ast::patternBoundNames(*params[i].pattern)) {
                seedOne(bound, Type::dynamic());
            }
            continue;
        }
        seedOne(params[i].name, t);
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
    // The same union lowering builds (docs/0020 decision 4): a name assigned
    // inside a `try` lives in an environment record, and `queries.h`'s rule
    // is that inference must believe about a variable exactly what lowering
    // decided about where it lives. Flow sensitivity on a cell would be
    // unsound — a handler can observe any of the writes.
    const auto captured = ast::getCapturedNames(body);
    scope.captured.insert(captured.begin(), captured.end());
    const auto tryAssigned = ast::getTryAssignedNames(body);
    scope.captured.insert(tryAssigned.begin(), tryAssigned.end());

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
        probe.runParamDefaults(params);
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
    recorder.runParamDefaults(params);
    recorder.runBody(body);
    if (mod.failed) return FunctionOutcome{Type::dynamic(), false};

    const Type returnType = recorder.inferredReturn(body);
    facts.signature.returnType = returnType;
    // A closure's only queryable proof (docs/0010 decision 5 excludes it
    // from signature specialization, so its parameters stay dynamic — but
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
