#pragma once
// The walker behind `analyzeFunction`, declared here because its two halves
// are written in two files: the statement walk and the fixpoint driver in
// flow.cpp, and the expression type rules in flow_expr.cpp.
//
// The seam is where the questions differ. A statement rule is about the
// ENVIRONMENT — what survives a merge, what a loop header has to converge to,
// what an abrupt exit leaves behind. An expression rule is about a VALUE —
// what type an operator produces, which callee a site contributes an argument
// to, which shape class a literal interns. They share a walker because the
// environment is threaded through the expressions, and nothing else.
//
// Not a public interface: nothing outside src/types includes this.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "types/flow.h"

namespace bronze::types {

constexpr size_t kNoSlot = static_cast<size_t>(-1);

// The pieces every structured loop has, so that `while`, `do-while`, `for`,
// `for-of` and `for-in` reach one fixpoint routine rather than five.
struct LoopParts {
    const ast::Expr* condition = nullptr;  // null for `for (;;)`
    const std::vector<ast::StmtPtr>* body = nullptr;
    const ast::Expr* update = nullptr;
    bool conditionFirst = true;  // false for do-while
};

// Walks one function body in source order carrying `name -> Type`
// flow-sensitively, per binding. Constructed fresh per pass; the `Scope` it
// works on outlives it, because the env-backed cells have to survive the flow
// fixpoint.
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

    // A parameter's default is CODE, evaluated in this function's scope on the
    // calls that omit the argument. Skipping it would hide every call site
    // inside it from the pass that widens callee signatures — the exact shape
    // of an unsound proof.
    void runParamDefaults(const std::vector<ast::Param>& params);

    Type inferredReturn(const std::vector<const ast::Stmt*>& body) const;

private:
    // ---- environment -------------------------------------------------------

    // Only the flow-sensitive bindings. The env-backed cells are reported
    // once for the whole function, since that is the granularity at which
    // they are true.
    const Env& visible() const { return scope_.env; }

    Type lookup(const std::string& name) const;
    void declare(const std::string& name, Type t);
    void assign(const std::string& name, Type t);

    // Does `name` resolve to anything the PROGRAM declared — a flow binding, a
    // captured cell, a module-level function? `false` means a read of it is a
    // global read. The builtin-identity proofs (`new Float64Array(...)` below)
    // hang off this rather than off `lookup`, because lookup answers with a
    // TYPE and `dynamic` cannot distinguish "user binding of unknown type"
    // from "not bound at all".
    bool resolvesToUserBinding(const std::string& name) const;

    // Is `e` the identifier `Math`, meaning the pristine builtin — the
    // program-wide bit says nothing can have changed it, and no binding in
    // scope shadows the name here?
    bool isPristineMathBase(const ast::Expr& e) const;
    // `Math.<own fn>(...)` with a pristine base — the calls whose value is a
    // Number by 21.3 whatever the arguments are.
    bool mathCallReturnsNumber(const ast::Call& c) const;

    // Every expression a pattern contains: the defaults, and the computed
    // keys. Both can call, so both are call sites this pass has to see.
    void patternDefaults(const ast::BindingPattern& pattern);

    // The names a pattern binds, all dynamic: they come out of an indexed or
    // keyed read, and this pass proves nothing about element or property types.
    void declarePattern(const ast::BindingPattern& pattern);

    // What a block-scoped statement list shadowed, so it can be put back.
    // Without this, `let x = 1; { let x = "s"; }` would leave the outer `x`
    // believing it is a string — an unsound narrowing, not a widening.
    //
    // `var` is deliberately absent: it is function-scoped wherever it is
    // written, which is exactly the line getScopeDeclarations draws.
    using ScopeSave = std::vector<std::pair<std::string, std::optional<Type>>>;

    ScopeSave saveDeclarations(const std::vector<std::string>& names);
    void restoreDeclarations(const ScopeSave& saved);

    // A template because the two statement-list representations (owning and
    // borrowed) are both walked, and inlining the save/restore around each
    // caller is how the shadowing bug above got in once already.
    template <typename List>
    void scopedStmtList(const List& stmts, uint32_t depth) {
        const ScopeSave saved = saveDeclarations(ast::getScopeDeclarations(stmts));
        stmtList(stmts, depth);
        restoreDeclarations(saved);
    }

    // A statement whose effects the AST does not expose invalidates every
    // binding. Widening to Dynamic is sound and is the designed fallback,
    // not a silent lie about semantics.
    void widenAll();

    // ---- dump recording ----------------------------------------------------

    size_t pushStmt(const char* label, uint32_t index, uint32_t depth);
    void pushMarker(const char* label, uint32_t depth);
    void closeStmt(size_t slot, const Env& before);

    // The join a merge point produced, keyed by the statement that owns it.
    // Recorded only on the final walk, like every other side-table entry, so
    // a probe pass's half-converged environment can never be what lowering
    // reads (`InferenceResult::typeOfBindingAt` is the contract).
    void recordMerge(const ast::Stmt& mergePoint, const Env& env);

    void fail(Span span, const std::string& what);

    // ---- statements (flow.cpp) ---------------------------------------------

    void stmtList(const std::vector<ast::StmtPtr>& stmts, uint32_t depth);
    void stmtList(const std::vector<const ast::Stmt*>& stmts, uint32_t depth);
    void stmt(const ast::Stmt& s, uint32_t index, uint32_t depth);
    void dispatch(const ast::Stmt& s, uint32_t depth);

    void keyedLoop(const ast::Stmt& s, const ast::Expr* source, const std::string& name,
                   const ast::BindingPattern* pattern, const std::vector<ast::StmtPtr>& body,
                   uint32_t depth);
    void switchStmt(const ast::SwitchStmt& sw, uint32_t depth);
    void tryStmt(const ast::TryStmt& t, uint32_t depth);
    void ifStmt(const ast::IfStmt& i, uint32_t depth);
    void analyzeLoop(const LoopParts& parts, uint32_t depth, const ast::Stmt& stmt);
    void runLoopOnce(const LoopParts& parts, const Env& header, uint32_t depth, bool record);

    // ---- expressions (flow_expr.cpp) ---------------------------------------

    Type expr(const ast::Expr& e);
    Type exprKind(const ast::Expr& e);
    Type unary(const ast::Unary& u);
    Type binary(const ast::Binary& b);
    Type call(const ast::Call& c);
    Type newExpr(const ast::NewExpr& n);
    ShapeClassId constructorShape(const std::string& name);
    Type objectLit(const ast::ObjectLit& o);
    // The arguments of `recv.name(...)` contributed to every method the call can
    // reach, and the type its dispatch produces. See method_ident.h.
    Type methodCall(const std::string& name, Type receiver, const std::vector<Type>& args,
                    bool spreadArgs);
    // The class a receiver type names, or null when it names none.
    const ClassLayout* receiverClass(Type receiver) const;
    // Records why a still-dynamic identifier receiver's parameter was refused,
    // when the identifier IS a parameter of the method this body is.
    void noteIdentRefusal(const ast::Ident& id, Type resolved);

    // `isGenerator` travels with the body for the reason flow.h gives.
    Type analyzeNested(const ast::Node& site, const std::string& declaredName,
                       const std::vector<ast::Param>& params,
                       const std::vector<ast::StmtPtr>& body, Span span,
                       bool isGenerator, ShapeClassId thisClass = kNoShapeClass,
                       uint32_t methodIndex = kNoMethod);
    void analyzeClassBody(const std::string& className,
                          const std::vector<ast::ClassMethod>& methods);

    ModuleContext& mod_;
    Scope& scope_;
    FunctionFacts& facts_;
    std::string qualifiedName_;
    bool record_ = false;

    Type returnAccum_ = Type::never();
    uint32_t anonCounter_ = 0;
    // The last member read this walker evaluated, and the type its BASE had.
    // A call site needs the receiver's type, and the callee expression has
    // already consumed it by the time `call` sees the result — re-walking the
    // base to get it back would evaluate its effects twice, so the one
    // evaluation leaves it here. One slot is enough: `expr(callee)` finishes
    // with the outermost member read, which is the one the call is on.
    const ast::MemberAccess* lastMember_ = nullptr;
    Type lastMemberBase_ = Type::dynamic();
    std::vector<std::vector<Env>> breakStack_;
    std::vector<std::vector<Env>> continueStack_;
    std::vector<Env> loopBreaks_;
    std::vector<Env> loopContinues_;
};

}  // namespace bronze::types
