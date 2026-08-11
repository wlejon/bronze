#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "support/diagnostics.h"
#include "types/result.h"

namespace bronze::lower {

// The AST -> IL pass. One instance per module; its methods are defined
// across the lower_*.cpp units named in the group comments below, each of
// which is one seam of the design it implements.
class Lowerer {
public:
    // `inference` may be null: that is the no-inference mode, and it
    // reproduces the pre-inference calling convention exactly (see lower.h).
    Lowerer(const ast::Module& astModule, DiagnosticSink& diags,
            const types::InferenceResult* inference)
        : astModule_(astModule), diags_(diags), inference_(inference) {}

    std::optional<il::Module> lower();

private:
    const ast::Module& astModule_;
    DiagnosticSink& diags_;
    // Never dereferenced outside lower_infer.cpp: every other unit asks the
    // accessors there, which answer "unproven" when this is null.
    const types::InferenceResult* inference_ = nullptr;
    il::Module ilModule_;
    std::unordered_map<std::string, uint32_t> functionIndices_;
    std::unordered_map<std::string, uint32_t> keyConstants_;
    uint32_t icSiteCounter_ = 0;

    struct Value {
        il::ValueId id;
        il::Type type;
    };

    struct VarBinding {
        std::string name;
        il::Type type = il::Type::Dynamic;
        bool isConst = false;
        bool isLet = false;
        bool isVar = false;
        bool isInitialized = true;
        uint32_t declOrder = 0;
        size_t scopeDepth = 0;
        il::ValueId valueId = il::kNoValue;
        // Captured by some nested function, so it lives in an environment
        // record instead of in SSA (docs/0007). Reads become env.get and
        // writes env.set, and it takes no part in SSA joins.
        bool inEnv = false;
        size_t envScopeIndex = 0;
        uint32_t envSlot = 0;
    };

    struct LoopContext {
        il::BlockId headerBlock = il::kNoBlock;
        il::BlockId updateBlock = il::kNoBlock;
        il::BlockId exitBlock = il::kNoBlock;
        std::vector<std::string> loopVars;
        // A loop whose update block takes a parameter that is not a source
        // binding — for-of's index (docs/0012 decision 2). `continue` jumps
        // to that block, so it has to hand the value over like any other
        // edge argument. kNoValue for every other loop form.
        il::ValueId updateExtraArg = il::kNoValue;
    };

    std::vector<VarBinding> varBindings_;
    std::unordered_map<std::string, size_t> activeVarMap_;
    size_t currentScopeDepth_ = 0;
    uint32_t varDeclCounter_ = 0;
    std::vector<LoopContext> loopStack_;
    size_t currentBlockIdx_ = 0;

    // --- environments (docs/0007) ---------------------------------------
    // One entry per open scope that declares a captured variable, innermost
    // last. The stack spans function boundaries: that is exactly how a
    // nested function resolves a free variable to a (depth, index) pair
    // relative to the environment it is handed at entry.
    struct EnvScopeInfo {
        std::unordered_map<std::string, uint32_t> slotOf;
        il::ValueId envValue = il::kNoValue;  // meaningful only in the owning function
    };
    std::vector<EnvScopeInfo> envScopes_;
    std::vector<il::ValueId> savedEnvValues_;
    std::vector<bool> scopeHasEnv_;
    il::ValueId currentEnvValue_ = il::kNoValue;
    // The `__this` parameter of the function being lowered, or kNoValue
    // where there is no receiver to speak of (docs/0008 decision 3).
    il::ValueId currentThisValue_ = il::kNoValue;
    // Lowering an arrow body, where `this` resolves through the environment
    // rather than to a parameter (docs/0012 decision 3).
    bool currentFunctionIsArrow_ = false;
    std::unordered_set<std::string> capturedNames_;
    size_t functionEnvBase_ = 0;   // envScopes_ size on entry to this function
    size_t functionEnvScope_ = SIZE_MAX;  // this function's own scope, if it has one

    // --- Conditional-expression joins -----------------------------------
    // &&, ||, ?? and ternary evaluate an operand on only some paths, so a
    // variable assigned inside such an operand needs a join parameter,
    // exactly like an if-statement arm. States are value snapshots because
    // assignments rebind varBindings_ entries in place.
    struct VarState {
        il::ValueId valueId;
        il::Type type;
    };
    using VarStateMap = std::unordered_map<std::string, VarState>;

    struct ExprJoin {
        std::vector<std::string> vars;
        std::unordered_map<std::string, il::ValueId> paramId;
        std::unordered_map<std::string, il::Type> paramType;
    };

    // --- lower_infer.cpp: what inference proved (docs/0010) --------------
    // The single place "there is no inference result" is answered, so no
    // other unit tests inference_ and --no-infer stays one null pointer
    // rather than a flag threaded through every site.
    static il::Type ilTypeOf(types::Type t);
    types::Type inferredType(const ast::Expr& expr) const;
    bool provenNumber(const ast::Expr& expr) const;
    bool monomorphicPropSite(const ast::Expr& receiver) const;
    il::Type mergeParamType(const ast::Stmt& mergePoint, const std::string& name) const;
    const types::Signature* provenSignature(uint32_t moduleFnIndex) const;
    types::Type provenParamType(uint32_t moduleFnIndex, size_t paramIndex) const;
    types::Type provenReturnType(uint32_t moduleFnIndex) const;
    types::Type provenClosureReturn(const ast::Node& site) const;
    bool applyProvenSignature(const ast::FunctionDecl& fnDecl, uint32_t moduleFnIndex,
                              il::Function& fn);
    // The annotation policy (docs/0010 decision 6). Returns false only for
    // annotation text bronze cannot read, which is a hard error; a hint that
    // no proof backs is a warning and compilation continues.
    bool checkAnnotation(const std::string& ann, Span span, const std::string& name,
                         types::Type proven);

    // --- lower.cpp: module skeleton and function bodies ------------------
    // The one rule for a function-level environment record, shared by real
    // function bodies and by the module top level lowered as `main`.
    void enterFunctionEnv(const std::vector<ast::Param>& params,
                          const std::vector<const ast::Stmt*>& body, il::Function& ilFn);
    bool lowerFunctionBody(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body, il::Function& ilFn);
    bool lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn);

    // --- lower_util.cpp: key constants, blocks, coercions, truthiness ----
    bool isProvidedGlobal(const std::string& name) const;
    uint32_t getKeyConstantIndex(const std::string& key);
    il::BlockId createBlock(il::Function& ilFn);
    void setCurrentBlock(size_t blockIdx);
    void emitInst(il::Function& ilFn, const il::Instruction& inst);
    bool currentBlockIsTerminated(const il::Function& ilFn) const;
    Value boxValueIfNeeded(Value val, il::Function& ilFn);
    Value unboxValueIfNeeded(Value val, il::Type targetType, il::Function& ilFn);
    Value emitCompoundCombine(Value cur, Value rhs, ast::BinaryOp binOp, bool provenNumeric,
                              il::Function& ilFn);
    Value coerceToType(Value val, il::Type target, il::Function& ilFn);
    Value lowerCondition(const ast::Expr& expr, il::Function& ilFn);
    Value lowerConditionFromVal(Value val, il::Function& ilFn);

    // --- lower_scope.cpp: scopes, environments, closures (docs/0007) -----
    bool declareVariable(const std::string& name, il::Type type, bool isConst, bool isLet,
                         bool isVar, bool isInitialized, il::ValueId valId, Span span);
    il::ValueId emitConstUndefined(il::Function& ilFn);
    il::ValueId emitEnvCreate(uint32_t slotCount, il::Function& ilFn);
    Value emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn);
    void emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn);
    uint32_t envDepthOf(size_t scopeIndex) const;
    Value readBinding(const VarBinding& b, il::Function& ilFn);
    void writeBinding(VarBinding& b, Value val, il::Function& ilFn);
    bool findEnclosingEnvVar(const std::string& name, uint32_t& depth, uint32_t& index) const;
    void enterScope();
    void enterScope(const std::vector<ast::StmtPtr>& stmts, il::Function& ilFn,
                    const std::string& extraDeclaration = std::string());
    void exitScope();
    // `site` is the AST node that IS the closure (a `FunctionExpr`, or a
    // nested `FunctionDecl` � docs/0007 decision 4 makes them one path). It
    // is how inference is asked about a function with no module index.
    // `isArrow` decides one thing only: where `this` inside the body comes
    // from (docs/0012 decision 3).
    std::optional<Value> lowerClosure(const ast::Node& site, const std::string& declaredName,
                                      const std::vector<ast::Param>& params,
                                      const std::string& returnTypeAnn,
                                      const std::vector<ast::StmtPtr>& body, Span span,
                                      il::Function& ilFn, bool isArrow = false);

    // --- lower_stmt.cpp: statements --------------------------------------
    bool lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);
    bool lowerStmt(const ast::Stmt& stmt, il::Function& ilFn);
    bool lowerVarDecl(const ast::VarDecl* varDecl, il::Function& ilFn);
    bool lowerReturnStmt(const ast::ReturnStmt* retStmt, il::Function& ilFn);

    // --- lower_control.cpp: control flow, block-argument SSA (docs/0005) -
    // One loop variable and the type every block parameter standing for it
    // takes — header, exit, and the update/condition join alike, because
    // the analysis proves one type covering all of them.
    struct LoopParam {
        std::string name;
        il::Type type = il::Type::Dynamic;
    };
    std::vector<il::ValueId> collectEdgeArgs(const std::vector<std::string>& vars,
                                             il::BlockId target, il::Function& ilFn);
    std::vector<std::string> getActiveVarsInDeclOrder() const;
    std::vector<LoopParam> collectLoopParams(const ast::Stmt& loopStmt,
                                             const std::unordered_set<std::string>& assigned);
    std::unordered_map<std::string, il::ValueId> addLoopBlockParams(
        const std::vector<LoopParam>& loopParams, il::BlockId block, il::Function& ilFn);
    void bindLoopBlockParams(const std::vector<LoopParam>& loopParams,
                             const std::unordered_map<std::string, il::ValueId>& paramOf);
    bool lowerIfStmt(const ast::IfStmt* ifStmt, il::Function& ilFn);
    bool lowerWhileStmt(const ast::WhileStmt* whileStmt, il::Function& ilFn);
    bool lowerDoWhileStmt(const ast::DoWhileStmt* doWhileStmt, il::Function& ilFn);
    bool lowerForStmt(const ast::ForStmt* forStmt, il::Function& ilFn);
    bool lowerForOfStmt(const ast::ForOfStmt* forOf, il::Function& ilFn);
    bool lowerBreakStmt(const ast::BreakStmt* breakStmt, il::Function& ilFn);
    bool lowerContinueStmt(const ast::ContinueStmt* continueStmt, il::Function& ilFn);

    // --- lower_expr_cond.cpp: conditional-expression joins (docs/0005) ---
    VarStateMap snapshotVarStates() const;
    void restoreVarStates(const VarStateMap& snap);
    ExprJoin makeExprJoin(const VarStateMap& a, const VarStateMap& b, il::BlockId joinBlock,
                          il::Function& ilFn);
    void appendExprJoinArgs(std::vector<il::ValueId>& args, const ExprJoin& join,
                            const VarStateMap& state, il::Function& ilFn);
    void bindExprJoinParams(const ExprJoin& join);
    std::optional<Value> lowerTernary(const ast::Ternary* tern, il::Function& ilFn);
    std::optional<Value> lowerLogical(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerNullish(const ast::Binary* bin, il::Function& ilFn);

    // --- lower_expr.cpp: expressions --------------------------------------
    std::optional<Value> lowerExpr(const ast::Expr& expr, il::Function& ilFn);
    std::optional<Value> lowerBinary(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerAssignment(const ast::Binary* bin, il::Function& ilFn);

    // --- lower_object.cpp: objects, property access, new, calls (docs/0008)
    std::optional<Value> lowerObjectLit(const ast::ObjectLit* objLit, il::Function& ilFn);
    std::optional<Value> lowerArrayLit(const ast::ArrayLit* arrLit, il::Function& ilFn);
    std::optional<Value> lowerNewExpr(const ast::NewExpr* newExpr, il::Function& ilFn);
    std::optional<Value> lowerMemberAccess(const ast::MemberAccess* mem, il::Function& ilFn);
    std::optional<Value> lowerIndexAccess(const ast::IndexAccess* idxAccess, il::Function& ilFn);
    std::optional<Value> lowerCall(const ast::Call* call, il::Function& ilFn);
};

}  // namespace bronze::lower
