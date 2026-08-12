#pragma once

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.h"
#include "types/shape_class.h"
#include "types/type.h"

namespace bronze::types {

// What inference proved about one module-level function's calling
// convention. Only meaningful when the function is direct-callable: every
// other function keeps the uniform dynamic convention, and its signature
// here is all-`Dynamic` to say exactly that.
struct Signature {
    std::vector<Type> params;  // source parameters only, in declaration order
    Type returnType = Type::never();
};

struct BindingChange {
    std::string name;
    Type type;
};

// One line of the canonical dump: a statement the analysis walked, and the
// bindings whose type it changed. `label` is the statement kind, or a
// structural marker ("then", "else", "body", "init") introducing the arms of
// a compound statement.
struct StatementFacts {
    uint32_t depth = 0;
    uint32_t index = 0;  // position in its own statement list
    bool isMarker = false;
    std::string label;
    std::vector<BindingChange> changes;  // sorted by name
};

struct FunctionFacts {
    std::string name;                    // qualified: "outer::inner" when nested
    uint32_t index = kNoFunctionIndex;   // module function index, if it has one
    std::vector<std::string> paramNames;
    Signature signature;
    bool directCallable = false;
    // Env-backed bindings and the single type joined over every write to each.
    // They are deliberately not in the per- statement diffs below: a cell is
    // one fact about the whole function, not a fact at a program point, because
    // a closure can write it at any time.
    std::vector<BindingChange> cells;
    std::vector<StatementFacts> statements;
};

// The side table lowering reads. Inference never mutates the AST and never
// rewrites IL; everything it proved is queried from here, keyed by AST node.
//
// The queries are the contract. The data members exist because the analysis
// has to fill them and the dump has to walk them; a consumer should not need
// to touch either.
struct InferenceResult {
    std::string moduleName;
    ShapeClassTable shapes;

    // Dump order: each module-level function in source order followed by the
    // functions nested inside it, then the module top level ("main") and its
    // nested functions.
    std::vector<FunctionFacts> functions;

    // Keyed by AST node identity. Pointer keys never reach an output path,
    // so their unordered iteration order cannot leak into the dump.
    std::unordered_map<const ast::Expr*, Type> exprTypes;
    std::unordered_map<const ast::Expr*, ShapeClassId> siteShapes;

    // Per merge point: the binding types the join at that point produced.
    // See `typeOfBindingAt` for what a merge point is and what the entry
    // covers. The inner map is ordered only so the analysis can compare
    // whole environments cheaply; nothing here reaches an output path.
    std::unordered_map<const ast::Stmt*, std::map<std::string, Type>> mergeBindings;

    // Per closure: the type its `return` statements were observed to produce,
    // keyed by the AST node that IS the closure — a `FunctionExpr`, or a nested
    // `FunctionDecl`, which desugars to one. See `closureReturnAt` for what
    // this is and is not.
    std::unordered_map<const ast::Node*, Type> closureReturns;

    std::map<std::string, uint32_t> moduleFunctionIndex;  // name -> index
    std::vector<uint32_t> moduleFunctionSlot;             // index -> `functions` slot
    std::vector<Signature> moduleSignatures;              // index -> signature
    std::vector<bool> moduleDirectCallable;               // index -> direct-callable

    // ---- the query interface lowering consumes ------------------------------

    // The type proven at one use site. Any expression the analysis did not
    // reach answers `Dynamic`, which is the designed sound fallback and never a
    // diagnostic.
    Type typeAt(const ast::Expr* expr) const;

    // The shape class of an object-creating site (an `ObjectLit` or a
    // `NewExpr`); `kNoShapeClass` when the site's identity is not proven.
    ShapeClassId shapeClassAt(const ast::Expr* site) const;

    // The type a binding is proven to hold at a control-flow MERGE POINT.
    //
    // A merge point is not an expression, so `typeAt` cannot express it, and it
    // is exactly what SSA joins need: a block parameter's type has to be an
    // upper bound of every edge that reaches it, including edges that lowering
    // has not built yet — the loop back edge is lowered after the header.
    // `mergePoint` is therefore the *statement that owns the merge*, which is a
    // node lowering holds in its hand when it creates the block:
    //
    //   - an `IfStmt`   — the join after the two arms;
    //   - a `WhileStmt` / `DoWhileStmt` / `ForStmt` — one answer covering
    //     every merge the loop builds: the header, the condition/update
    //     block that the body fall-through and `continue` edges meet in, and
    //     the exit block. They get one answer because the analysis's loop
    //     fixpoint gives one: the converged header already subsumes the
    //     entry, the end of the body and every `continue`, and the recorded
    //     type joins the `break` environments on top, so it is an upper
    //     bound of every value that can flow along any of those edges.
    //
    // A name the analysis did not prove anything about at that point — a
    // binding it never saw, a block-scoped name that did not survive the
    // join, an env-backed cell (which is memory, not SSA, and never a block
    // parameter) — answers `Dynamic`, the designed sound fallback.
    Type typeOfBindingAt(const ast::Stmt* mergePoint, const std::string& name) const;

    // By module function index — the position among the top-level
    // `FunctionDecl`s, which is the numbering lowering already assigns.
    //
    // A `Never` in here is not a mistake and not an error: it means no value
    // ever reaches that position, i.e. the function is direct-callable and
    // has no call sites at all. A consumer must decide what to do with a
    // dead function rather than map `Never` onto an IL type — there isn't
    // one. Every function that is not direct-callable reports the uniform
    // dynamic convention instead, so `Never` and `isDirectCallable` are
    // always read together.
    const Signature& signatureOf(uint32_t functionIndex) const;
    bool isDirectCallable(uint32_t functionIndex) const;

    // A closure's proof surface, and the whole of it: what its body was
    // observed to RETURN. A closure has no module function index, so
    // `signatureOf` cannot speak for one; this is keyed by the AST node
    // instead, which is what a consumer holds when it lowers the closure.
    //
    // Deliberately the return only. A closure's PARAMETERS have no proof and
    // cannot get one here: a signature is inferred by joining over all call
    // sites, which is sound only for a name whose callers this compilation can
    // enumerate, and a closure is reached through a function value. So its
    // parameters keep the uniform dynamic convention and this reports nothing
    // about them — an absence by design, not a gap. Its return, by contrast, is
    // a fact about the body alone: the analysis already walks it and joins
    // every `return`.
    //
    // This never types anything. It exists so a consumer can tell a closure
    // annotation that agrees with the body from one that does not; the calling
    // convention is dynamic either way. A closure the analysis did not reach
    // answers `Dynamic`, the designed sound fallback.
    Type closureReturnAt(const ast::Node* site) const;

    bool isDirectCallable(const std::string& name) const;
    std::optional<uint32_t> functionIndexOf(const std::string& name) const;
    uint32_t moduleFunctionCount() const {
        return static_cast<uint32_t>(moduleSignatures.size());
    }
};

}  // namespace bronze::types
