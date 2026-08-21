#pragma once

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "types/class_layout.h"
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

    // Every `class` in the program: its identity, and — where the whole
    // construction sequence was modellable — the slot each instance field
    // lands in. See class_layout.h for the two-tier claim this carries.
    ClassLayoutTable classLayouts;

    // Dump order: each module-level function in source order followed by the
    // functions nested inside it, then the module top level ("main") and its
    // nested functions.
    std::vector<FunctionFacts> functions;

    // Keyed by AST node identity. Pointer keys never reach an output path,
    // so their unordered iteration order cannot leak into the dump.
    std::unordered_map<const ast::Expr*, Type> exprTypes;
    std::unordered_map<const ast::Expr*, ShapeClassId> siteShapes;

    // Why an identifier that names a METHOD PARAMETER is still dynamic, for the
    // one identifier read that asked. Diagnostics only — `--infer-stats` splits
    // the "receiver is dynamic: identifier" row by it, which is how a chunk aimed
    // at those sites finds out which refusal is holding them and how many. No
    // consumer types anything from it.
    std::unordered_map<const ast::Expr*, std::string> identRefusals;

    // Per merge point: the binding types the join at that point produced.
    // See `typeOfBindingAt` for what a merge point is and what the entry
    // covers. The inner map is ordered only so the analysis can compare
    // whole environments cheaply; nothing here reaches an output path.
    std::unordered_map<const ast::Stmt*, std::map<std::string, Type>> mergeBindings;

    // Call sites proven to reach a pristine builtin `Math` method: the
    // module-wide taint scan says nothing can have changed `Math`, and no
    // binding in scope shadows the name at this site. What the proof licenses
    // is skipping the EVALUATION of `Math.<fn>` — the global read, the
    // property load and the call-shaped guards — not just typing the result;
    // that is why the site itself is recorded and not only its type.
    std::unordered_set<const ast::Expr*> pristineMathCalls;

    // Every property read whose `Number` came from the AUDITED field path: the
    // receiver was watched being made, the class installs the field on every
    // construction path with no accessor over it, and the whole program writes
    // nothing but Numbers into that name (types/field_audit.h).
    //
    // Recorded as a site rather than left to `typeAt` because the two are
    // different claims. `typeAt` answering `Number` is the ordinary lattice
    // fact, and every consumer of it converts through a CHECKED unbox, which is
    // ToNumber and therefore harmless if the claim is somehow wrong. A site in
    // here additionally licenses the RAW unbox — a bitcast with no tag test and
    // no helper — which is harmless only if the claim is right. So the stronger
    // licence is granted by name, to the one path that carries the proof, and
    // is not inherited by every expression that happens to type `number`.
    std::unordered_set<const ast::Expr*> provenFieldReads;

    // What the write audit decided, summarized for `--infer-stats`. The audit
    // itself is a fixpoint-local table (`ModuleContext::fieldAudit`); this is
    // the part of it a report needs after inference has returned.
    struct FieldAuditReport {
        uint32_t namesWritten = 0;
        uint32_t namesClean = 0;
        // Names carrying no refusal of their OWN. Equal to `namesClean` unless
        // one whole-program construct stood everything down, and then it is the
        // size of what that one construct cost.
        uint32_t namesLocallyClean = 0;
        std::vector<std::string> cleanNames;
        std::map<std::string, uint32_t> globalRefusals;  // reason -> sites
        std::map<std::string, uint32_t> refusals;  // reason -> names refused for it
        // The read-site population, so the report can say what the audit moved
        // rather than only what it certified. Every property read whose base
        // carries a shape class and whose class harvest says `number`, split by
        // what stopped it.
        uint32_t numberFieldReads = 0;
        uint32_t refusedNotBuiltHere = 0;
        uint32_t refusedByClass = 0;
        uint32_t refusedByAudit = 0;
        // `o[k] = v` and `delete o[k]`: how many the program contains, how many
        // the flow pass could not prove harmless, and what those ones' KEYS were
        // typed. One global refusal decides the whole audit, so this is the row
        // that says whether the next move is more key types or something else
        // entirely.
        uint32_t computedSites = 0;
        uint32_t computedRefuted = 0;
        std::map<std::string, uint32_t> computedKeyTypes;
        std::map<std::string, uint32_t> computedReceiverTypes;

        struct ResidueSite {
            std::string reason;
            uint32_t count = 0;
            std::string representativeSite;
        };
        std::vector<ResidueSite> residue;
    };
    FieldAuditReport fieldAudit;

    // What the method-parameter join decided, summarized for `--infer-stats`.
    struct MethodParamReport {
        uint32_t classes = 0;
        uint32_t methods = 0;
        uint32_t params = 0;
        uint32_t paramsNumber = 0;
        uint32_t paramsObject = 0;
        uint32_t paramsOther = 0;
        uint32_t paramsDynamic = 0;
        uint32_t methodsSpeaking = 0;
        uint32_t methodsUnreached = 0;
        uint32_t methodsNotPlain = 0;
        std::map<std::string, uint32_t> poisons;
        std::string globalPoison;
        uint32_t unboundedCalls = 0;
    };
    MethodParamReport methodParams;

    // What the constructor-parameter join decided, summarized for
    // `--infer-stats` (types/ctor_ident.h). The chain this chunk has to move
    // starts here: parameters typed, then the field harvest that reads them,
    // then the audit, then the raw loads.
    struct CtorParamReport {
        uint32_t classes = 0;   // named classes in the program
        uint32_t ctors = 0;     // of those, ones declaring a constructor
        uint32_t params = 0;    // their parameters, all together
        uint32_t paramsNumber = 0;
        uint32_t paramsObject = 0;
        uint32_t paramsOther = 0;    // proven, but neither a number nor an object
        uint32_t paramsDynamic = 0;
        uint32_t ctorsSpeaking = 0;  // neither poisoned nor unreached nor unplain
        uint32_t ctorsUnreached = 0;
        uint32_t ctorsNotPlain = 0;
        // `constructor(...args) { super(...args) }`, which the parser
        // synthesizes for every derived class that declares none. A link in the
        // chain rather than a constructor with parameters of its own.
        uint32_t forwarders = 0;
        std::map<std::string, uint32_t> poisons;  // reason -> classes it stood down
        std::string globalPoison;  // non-empty when every class stood down at once
        // Whether a constructor VALUE can be in circulation without its class
        // binding having been read, which is what decides whether a `new` this
        // pass cannot name has to contribute to every class in the program.
        bool valueEscapes = false;
        std::string valueEscapeReason;
        // `new <a value>(...)` sites, by how far the analysis could bound them:
        // to the receiver's class subtree (`new x.constructor()`), to nothing at
        // all (every class such a site can reach is already poisoned), or to the
        // whole program — the last being the row that says whether one unnameable
        // construction is costing every class in the library its parameters.
        uint32_t unnamedNewSubtree = 0;
        uint32_t unnamedNewIgnored = 0;
        uint32_t unnamedNewAll = 0;
    };
    CtorParamReport ctorParams;

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

    // The instance slot `field` occupies on a receiver of the type proven at
    // `receiver`, or `ClassLayoutTable::kNoSlot`.
    //
    // The answer is a claim about LAYOUT, which only a class whose whole
    // construction sequence was modellable makes — an identity alone is never
    // enough. `kNoSlot` for every receiver whose type is not a proven-layout
    // class, and for every key that class does not install on the instance
    // (a prototype method, an inherited accessor, an absent name).
    uint32_t staticSlotAt(const ast::Expr* receiver, const std::string& field) const;

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
