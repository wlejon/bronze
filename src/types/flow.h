#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "support/diagnostics.h"
#include "types/result.h"
#include "types/type.h"

namespace bronze::types {

// The lattice is three tall (Never -> a concrete kind -> Dynamic), so every
// fixpoint here settles in a couple of rounds. These bounds are tripwires
// for a rule that stopped being monotone, not real limits: exceeding one is
// an internal impossibility and is diagnosed rather than looped on.
inline constexpr uint32_t kMaxFlowIterations = 8;
inline constexpr uint32_t kMaxCallGraphIterations = 32;

// `name -> Type` at one program point.
using Env = std::map<std::string, Type>;

// One module-level function as the call-graph fixpoint sees it.
struct FunctionInfo {
    const ast::FunctionDecl* decl = nullptr;
    std::string name;
    // The current estimate of the calling convention. Starts at `Never` for
    // a direct-callable function and only widens, which is what makes
    // recursion converge (decision 5); all-`Dynamic` and frozen otherwise.
    Signature signature;
    bool directCallable = false;
    // Joined over the call sites seen in the pass now running, then folded
    // into `signature` at the end of it.
    std::vector<Type> observedParams;
    Type observedReturn = Type::never();
};

// One function's binding state, chained through `parent` for closures.
//
// A closure resolves outer names against the enclosing scope's `cells` and
// never against its `env`: a name a nested function mentions is env-backed
// by construction (docs/0007 decision 1), so anything still in `env` is
// genuinely not visible from here.
struct Scope {
    Scope* parent = nullptr;
    Env env;                        // flow-sensitive, per program point
    Env cells;                      // env-backed, one cell joined over the function
    std::set<std::string> captured;
};

struct ModuleContext {
    std::vector<FunctionInfo> functions;      // by module function index
    std::map<std::string, uint32_t> indexByName;
    InferenceResult* result = nullptr;
    DiagnosticSink* diags = nullptr;
    // Shape class per constructor, computed once from its `this.x = ...`
    // assignments. Keyed by module function index.
    std::map<uint32_t, ShapeClassId> ctorShapes;
    bool failed = false;
};

struct FunctionOutcome {
    Type returnType = Type::undefined();
    bool ok = true;
};

// Analyses one function body to fixpoint over its env-backed cells and
// returns the type its `return` statements produce.
//
// `record` fills the result side table and appends this function's
// `FunctionFacts`; the call-graph fixpoint's probe passes leave it off, so
// each expression is recorded exactly once, on the final pass.
//
// `site` is the AST node that IS this function when it is a closure — a
// `FunctionExpr` or a nested `FunctionDecl` — and null for a module-level
// function (which `moduleIndex` names instead) and for the module top level.
// It is the key `InferenceResult::closureReturnAt` answers on, which is the
// only handle a closure has: it has no module function index.
FunctionOutcome analyzeFunction(ModuleContext& mod, Scope* parent,
                                const std::string& qualifiedName, uint32_t moduleIndex,
                                const ast::Node* site, bool directCallable,
                                const std::vector<ast::Param>& params,
                                const std::vector<Type>& paramTypes,
                                const std::vector<const ast::Stmt*>& body, Span span,
                                bool record);

// The `Type` of joining two program points: names in both are joined, names
// in only one are dropped (they are block-scoped declarations that did not
// survive the merge).
Env joinEnv(const Env& a, const Env& b);

}  // namespace bronze::types
