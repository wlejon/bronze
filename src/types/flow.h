#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "support/diagnostics.h"
#include "types/class_layout.h"
#include "types/field_audit.h"
#include "types/method_ident.h"
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
    // The current estimate of the calling convention. Starts at `Never` for a
    // direct-callable function and only widens, which is what makes recursion
    // converge; all-`Dynamic` and frozen otherwise.
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
// never against its `env`: a name a nested function mentions is env-backed by
// construction, so anything still in `env` is genuinely not visible from here.
struct Scope {
    Scope* parent = nullptr;
    Env env;                        // flow-sensitive, per program point
    Env cells;                      // env-backed, one cell joined over the function
    std::set<std::string> captured;
    // The class whose instances `this` names in this body, or `kNoShapeClass`.
    //
    // Set for a non-static class method, accessor, field initializer and
    // constructor, and inherited by arrow functions nested in one (they close
    // over the enclosing `this`, 15.3.4). Every other body — a plain function
    // declaration, a non-arrow function expression, a static method, the module
    // top level — leaves it unset, because `this` there is whatever the CALL
    // passed and no declaration can speak for it.
    //
    // This is an OPTIMISTIC claim, not a proof: `Vector3.prototype.add.call(x)`
    // makes `this` an `x`, and nothing here can see that. It is safe for the
    // same reason every other shape claim in this file is — what consumes it
    // compares the shape word at run time. Note the standing invariant that
    // makes that true: `TypeKind::Object` licenses NOTHING but the property-site
    // form. `typeof` answers `string` for any operand (operator_types.cpp),
    // truthiness is not folded from types, and `ilTypeOf` maps every non-Number
    // to `Dynamic`. A future rule that folds a branch or a `typeof` from
    // Object-ness would have to stop believing this field first.
    ShapeClassId thisClass = kNoShapeClass;
    // The class method this body IS, when it is one. Read for two things: the
    // enclosing class of a `super.m()` call, and the stats row that attributes a
    // still-dynamic identifier receiver to the reason its parameter was refused.
    uint32_t methodIndex = kNoMethod;
};

struct ModuleContext {
    std::vector<FunctionInfo> functions;      // by module function index
    std::map<std::string, uint32_t> indexByName;
    InferenceResult* result = nullptr;
    DiagnosticSink* diags = nullptr;
    // Shape class per constructor, computed once from its `this.x = ...`
    // assignments. Keyed by module function index.
    std::map<uint32_t, ShapeClassId> ctorShapes;
    // Every name the MODULE SCOPE binds: lexical and hoisted declarations plus
    // import locals. A function body's scope chain here stops at the function
    // (its parent is null), so this is the only way `resolvesToUserBinding`
    // can tell a read of a module-level `const Float64Array` from a read of
    // the global builtin — and the builtin-identity proofs need exactly that
    // distinction.
    std::set<std::string> moduleScopeNames;
    // No statement in the whole (merged) program can change what `Math`
    // means: the global is unassignable by compile error, no bare `Math`
    // escapes member-read position, no member write/update/delete goes
    // through it, `globalThis` is never mentioned, and no host manifest
    // overrides either name. Under this bit `Math.sqrt(x)` IS the builtin,
    // and the builtin returns a Number for any arguments — which is the
    // whole licence for typing those calls (flow_expr.cpp).
    bool mathPristine = false;
    bool failed = false;

    // ---- interprocedural identity (method_ident.h) --------------------------

    // Every class method, and the `extends` forest that decides which of them a
    // call on a given receiver class can reach.
    MethodTable methods;
    // Which method names have given up their parameters, and why. Grows only:
    // an escape is a fact about the program text, and a call on a receiver whose
    // class is not proven stays unproven, because a receiver's type only widens.
    MethodPoison methodPoison;
    // `BRONZE_NO_INTERPROC_IDENT` turns the whole mechanism off, leaving every
    // method's parameters on the uniform dynamic convention.
    bool interprocIdent = false;

    // ---- value flow through module-scope bindings ---------------------------

    // What a name the MODULE SCOPE binds holds, joined over every declaration
    // and every assignment anywhere in the program.
    //
    // The scope chain cannot answer this. A module-level function's `Scope` has
    // no parent (infer.cpp passes null), and the module top level is analysed as
    // a separate body with a scope of its own — so a method that reads
    // `_vector`, which three.js declares once at module scope and shares across
    // the whole library, resolves nothing and reads `Dynamic`. The binding is
    // one cell for the whole program, though, so its contents CAN be joined
    // program-wide: that is what this is, and it is the same shape of fact as
    // `Scope::cells`, one scope further out than any scope chain reaches.
    //
    // The answer is handed out as `Type::objectIdentityOnly` and never as a
    // fact, because this table says what the binding holds SOMEWHERE and not
    // what it holds at the read. A `let` still in its temporal dead zone, or one
    // whose `new C()` runs after the function that reads it, are both reads this
    // has no order for — and an identity is exactly the claim that survives
    // being wrong, since a shape guard checks it (types/type.h).
    //
    // A read this answers for might not be a read of the module binding at all:
    // a `var` hoisted into some function body, or a nested declaration of the
    // same name, is `undefined` at a read that comes before the declaring
    // statement, and this pass resolves that read by name. Deliberately not
    // scanned for, because the consequence is bounded by what the answer
    // LICENSES: an object identity, checked by a shape compare the runtime
    // performs, whose cost when wrong is a miss. It is the same trade
    // `Scope::thisClass` records, and the same reason a primitive is never
    // answered from here — a `number` would be an unguarded claim about a value,
    // and this table has no program order to justify one with.
    std::map<std::string, Type> moduleBindings;
    // `BRONZE_NO_VALUE_FLOW` turns the table off, leaving every module-scope
    // read `Dynamic` as it was.
    bool valueFlow = false;

    // ---- the field-type write audit (field_audit.h) -------------------------

    // Which property names the whole program only ever writes Numbers into.
    // Part of the same fixpoint as the signatures: a write's type depends on
    // whether the fields it reads are clean, and whether a field is clean
    // depends on the types of the writes. Refutation is monotone, so the two
    // settle together. There is NO seam for this one — it is what makes a
    // primitive field type a proof rather than a guess, and a compiler with it
    // switched off miscompiles.
    FieldAudit fieldAudit;
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
// `isGenerator` is not a detail of the body: ECMA-262 27.5.1.2 makes CALLING a
// generator function build a generator object and run none of the body, so what
// the body returns is the `value` of a final result and never the call's value.
// Inference that read the `return` statements would hand every caller the wrong
// type — see the note at the override in flow.cpp.
FunctionOutcome analyzeFunction(ModuleContext& mod, Scope* parent,
                                const std::string& qualifiedName, uint32_t moduleIndex,
                                const ast::Node* site, bool directCallable,
                                const std::vector<ast::Param>& params,
                                const std::vector<Type>& paramTypes,
                                const std::vector<const ast::Stmt*>& body, Span span,
                                bool record, bool isGenerator = false,
                                ShapeClassId thisClass = kNoShapeClass,
                                uint32_t methodIndex = kNoMethod,
                                bool moduleTopLevel = false);

// The `Type` of joining two program points: names in both are joined, names
// in only one are dropped (they are block-scoped declarations that did not
// survive the merge).
Env joinEnv(const Env& a, const Env& b);

}  // namespace bronze::types
