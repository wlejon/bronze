#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "lower/infer_stats.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/result.h"

namespace bronze::lower {

// The AST -> IL pass. One instance per module; its methods are defined
// across the lower_*.cpp units named in the group comments below, each of
// which is one seam of the design it implements.
class Lowerer {
public:
    // `inference` may be null: that is the no-inference mode, and it
    // reproduces the pre-inference calling convention exactly (see lower.h).
    // `hostGlobals` may be null too — no manifest — and is copied into a set
    // here because isProvidedGlobal is asked once per free-identifier mention.
    Lowerer(const ast::Module& astModule, DiagnosticSink& diags,
            const types::InferenceResult* inference,
            const std::vector<std::string>* hostGlobals = nullptr,
            const SourceSet* sources = nullptr,
            InferStatsCollector* stats = nullptr)
        : astModule_(astModule), diags_(diags), inference_(inference),
          sources_(sources), stats_(stats) {
        if (hostGlobals) hostGlobals_.insert(hostGlobals->begin(), hostGlobals->end());
        if (stats_ && sources_) stats_->setSourceSet(sources_);
    }

    std::optional<il::Module> lower();

private:
    const ast::Module& astModule_;
    DiagnosticSink& diags_;
    // Never dereferenced outside lower_infer.cpp: every other unit asks the
    // accessors there, which answer "unproven" when this is null.
    const types::InferenceResult* inference_ = nullptr;
    il::Module ilModule_;
    // Names a `--host-globals` manifest admitted: the open half of the
    // provided-globals set. isProvidedGlobal consults it after the builtin
    // list, so a read of one lowers to `global.get` like a builtin's does.
    std::unordered_set<std::string> hostGlobals_;
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
        // record instead of in SSA. Reads become env.get and writes env.set,
        // and it takes no part in SSA joins.
        bool inEnv = false;
        size_t envScopeIndex = 0;
        uint32_t envSlot = 0;
        // The binding this declaration displaced in `activeVarMap_`, if it
        // shadowed one. A block's declarations are discarded on exit and the
        // enclosing scope's are NOT (ECMA-262 14.2.2), so leaving the name
        // simply erased made `let x = 1; { let x = 2; } x` report
        // `undefined variable: x` — the inner declaration destroyed the outer
        // binding instead of hiding it.
        size_t shadowedBinding = SIZE_MAX;
        // Created by scope entry rather than by a declaration: the binding
        // exists and holds the uninitialized marker, which is what makes a read
        // above the declaration resolve HERE (and throw) instead of reaching
        // out to whatever the enclosing scope calls the name. The declaration,
        // when lowering reaches it, initializes this binding rather than making
        // a second one — and clears the flag, so a genuine `let x; let x;` is
        // still the redeclaration error it was.
        bool isTdzHoisted = false;
    };

    // Where a `break` or a `continue` goes. ONE stack for all three kinds,
    // because the two statements search the same entries by different rules:
    // an unlabelled `break` stops at the innermost *breakable* statement (a
    // loop OR a switch), an unlabelled `continue` at the innermost
    // *iteration* statement, and a labelled one at the entry carrying its
    // label whatever kind that entry is. Two stacks would have to agree about
    // nesting order, and the whole content of `break outer` is that order.
    enum class JumpKind { Loop, Switch, LabeledBlock };

    struct JumpTarget {
        JumpKind kind = JumpKind::Loop;
        // The label this statement was written under, or empty. A label is
        // not a binding: it is only ever compared, never resolved.
        std::string label;
        il::BlockId headerBlock = il::kNoBlock;
        // kNoBlock for anything but a loop — which is exactly why
        // `continue lbl` naming a switch or a block is an early error rather
        // than a jump to nowhere.
        il::BlockId updateBlock = il::kNoBlock;
        il::BlockId exitBlock = il::kNoBlock;
        // The variables the target's blocks take as parameters, in the order
        // those parameters were added. A jump from anywhere inside has to
        // hand over the same list.
        std::vector<std::string> vars;
        // Where `cleanupStack_` stood when this statement was reached, and
        // where it stood once the statement's OWN cleanup (a for-of's
        // IteratorClose) was on it. The two differ for exactly one form, and
        // the difference is the whole of "a `break` closes the iterator and a
        // `continue` does not": both jumps cross the same finallys, and only
        // the break crosses the loop's own close.
        size_t cleanupDepthAtEntry = 0;
        size_t cleanupDepthInBody = 0;
        // The `for` loop's per-iteration environment record (14.7.4.9), which
        // its update block takes as one more parameter than its variables. A
        // `continue` is an edge into that block and so has to hand it over
        // too, and the value is the loop header's — not whatever record is
        // innermost where the `continue` is written, which may be several
        // blocks deep inside the body. `kNoValue` for every other statement,
        // and for a `for` whose head binds nothing a closure reaches.
        il::ValueId perIterationEnv = il::kNoValue;
    };

    std::vector<VarBinding> varBindings_;
    std::unordered_map<std::string, size_t> activeVarMap_;
    size_t currentScopeDepth_ = 0;
    uint32_t varDeclCounter_ = 0;
    std::vector<JumpTarget> jumpStack_;
    // The labels currently in scope, innermost last. Separate from
    // `jumpStack_` because the duplicate-label early error has to fire BEFORE
    // the labelled statement is lowered, and a loop pushes its jump target
    // only once lowering reaches it.
    std::vector<std::string> labelStack_;
    // The innermost enclosing `try`'s handler block, stamped onto every block
    // `createBlock` makes while it is set. `kNoBlock` means "an exception
    // here leaves the function", which is what a body outside any `try` wants
    // and what the backend turns into a frame pop and a `ret`.
    il::BlockId currentHandler_ = il::kNoBlock;

    // The work an abrupt completion has to do on its way out, innermost last.
    // Two kinds, and they are one stack because they interleave: `break outer`
    // from inside `for (const x of it) { try { ... } finally { ... } }` runs
    // the finally and THEN closes the iterator, and only their relative order
    // on one stack says so.
    //
    // Each records the `jumpStack_` depth it was pushed at, which is the whole
    // of "does this `break` cross it?": a `break` to the target at index i
    // crosses every entry pushed when the stack was deeper than i.
    //
    // Function-local, and saved/cleared/restored across a function boundary
    // exactly as `labelStack_` is: a `return` inside a nested function runs
    // that function's cleanups and none of the enclosing ones.
    enum class CleanupKind {
        // The `finally` body, lowered again here, once per exit path.
        Finally,
        // IteratorClose on a for-of left early.
        IteratorClose,
        AsyncIteratorClose,
    };

    struct CleanupFrame {
        CleanupKind kind = CleanupKind::Finally;
        const ast::TryStmt* stmt = nullptr;      // Finally only
        il::ValueId iterRecord = il::kNoValue;   // IteratorClose only
        // Where that record lives in a MACHINE body's frame, or UINT32_MAX
        // for a loop in ordinary code. A `break` out of a suspending loop can
        // be reached from a resume block, where the SSA value above was never
        // defined — so the close has to re-read the record from the frame.
        uint32_t iterFrameSlot = UINT32_MAX;     // IteratorClose only
        size_t jumpDepth = 0;
        // The handler in effect OUTSIDE this try. Every copy of the finally
        // body runs under it, never under the try's own handler: an exception
        // the finally raises propagates outward, and a block still naming the
        // try's handler would re-enter it and run the same finally a second
        // time.
        il::BlockId outerHandler = il::kNoBlock;
    };
    std::vector<CleanupFrame> cleanupStack_;

    // The label a `label:` just read, waiting for the loop or switch it
    // fronts to claim it. Cleared by whichever statement lowering reaches
    // next, so a label can never leak onto a second statement.
    std::string pendingLabel_;
    size_t currentBlockIdx_ = 0;

    // --- environments --------------------------------------- One entry per
    // open scope that declares a captured variable, innermost last. The stack
    // spans function boundaries: that is exactly how a nested function resolves
    // a free variable to a (depth, index) pair relative to the environment it
    // is handed at entry.
    struct EnvScopeInfo {
        std::unordered_map<std::string, uint32_t> slotOf;
        // Slot -> the name it holds. A checked read names the binding it
        // refused, and a vector indexed by slot is the one form of that
        // mapping no iteration order can reach.
        std::vector<std::string> slotNames;
        // Which slots hold a `let`, `const` or `class` binding — the ones that
        // start out uninitialized and whose every read is 9.1.1.1.6's check.
        // A parameter, a `var`, a hoisted `function` and the synthetic `this`
        // and `arguments` slots are never among them.
        std::vector<bool> slotIsLexical;
        il::ValueId envValue = il::kNoValue;  // meaningful only in the owning function
        // GENERATORS ONLY: the slot in THIS record that holds the record of the
        // scope nested directly inside it. The environment chain runs upward —
        // `env.get` walks parents — so from the frame there is no way down, and
        // a generator needs one: a resume edge defines no SSA value, so the
        // record innermost at that point cannot be a value carried in from
        // wherever it was created. See `currentEnv`.
        uint32_t childSlot = UINT32_MAX;
    };
    std::vector<EnvScopeInfo> envScopes_;
    std::vector<il::ValueId> savedEnvValues_;
    std::vector<bool> scopeHasEnv_;
    il::ValueId currentEnvValue_ = il::kNoValue;
    // The record innermost right here, as a value usable in the block being
    // emitted into. `currentEnvValue_` itself outside a generator, where a
    // record's defining instruction dominates every use of it; inside one, a
    // fresh walk DOWN from the resume function's frame parameter through the
    // child links, because the resume dispatch's edge into the middle of the
    // body dominates nothing. Costs one load per level of block nesting, in
    // generators only.
    il::ValueId currentEnv(il::Function& ilFn);
    // The `__this` parameter of the function being lowered, or kNoValue where
    // there is no receiver to speak of.
    il::ValueId currentThisValue_ = il::kNoValue;
    // Lowering an arrow body, where `this` resolves through the environment
    // rather than to a parameter.
    bool currentFunctionIsArrow_ = false;
    // Whether the code being lowered is STRICT (ECMA-262 11.2.2). Read off the
    // AST node the parser wrote it onto — a module or a function — and saved
    // and restored across every nested body: strictness only ever rises on the
    // way in, but a sloppy function lowered after a strict sibling must not
    // inherit its neighbour's mode.
    //
    // The one thing it selects here is `strictFlag()`, which travels on every
    // write instruction. It has to be per-INSTRUCTION and not per-IL-function
    // because a single IL function can be neither: `main` holds the module's
    // top level, and a strict function's body and a sloppy one's are separate
    // IL functions but reach the same helper.
    bool strictCode_ = false;
    int32_t strictFlag() const { return strictCode_ ? 1 : 0; }
    std::unordered_set<std::string> capturedNames_;
    // Every binding of this function that may not live in SSA: `capturedNames_`
    // (a closure can read it after the declaring scope's SSA values are gone)
    // plus every name assigned inside a `try` (a handler is entered from a
    // point no join can enumerate). One set, because `enterScope` and
    // `enterFunctionEnv` ask one question — "does this name need an environment
    // slot?" — and the two reasons have the same answer.
    //
    // Deliberately NOT the set `lowerForStmt` copies per iteration: a slot is
    // one thing and 14.7.4.9's copy is another. `try { for (let i = 0; ...) }`
    // puts `i` in a record because a handler may read it, and copying that
    // record every time round would be allocation no closure can observe.
    std::unordered_set<std::string> memoryNames_;
    size_t functionEnvBase_ = 0;   // envScopes_ size on entry to this function
    size_t functionEnvScope_ = SIZE_MAX;  // this function's own scope, if it has one
    // The module scope. Its slot layout is decided before ANY body is lowered,
    // because a top-level function declaration resolves module-level names
    // against it and is lowered long before `main` exists to create the record.
    // It sits at the bottom of `envScopes_` for the whole compilation and is
    // never popped, so every (depth, index) pair anywhere in the module counts
    // hops to the same place.
    std::vector<std::string> moduleEnvSlots_;
    size_t moduleEnvScope_ = SIZE_MAX;

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

    // --- lower_infer.cpp: what inference proved -------------- The single
    // place "there is no inference result" is answered, so no other unit tests
    // inference_ and --no-infer stays one null pointer rather than a flag
    // threaded through every site.
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
    // The annotation policy. Returns false only for annotation text bronze
    // cannot read, which is a hard error; a hint that no proof backs is a
    // warning and compilation continues.
    bool checkAnnotation(const std::string& ann, Span span, const std::string& name,
                         types::Type proven);

    std::string propBailReason(const ast::Expr& expr) const;
    void recordPropertyAccess(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    void recordCall(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    void recordElementOp(uint16_t fileId, bool isNative, const std::string& bailReason = "");

    const SourceSet* sources_ = nullptr;
    InferStatsCollector* stats_ = nullptr;

    // --- lower_generator.cpp: the generator state machine ----------------
    // What the resume function needs to know about the frame it was closed
    // over. Set only while a resume body is being lowered, and saved and
    // restored across every nested closure exactly as the rest of the
    // per-function state is: a function written inside a generator is not one.
    struct GeneratorContext {
        size_t frameScope = SIZE_MAX;         // index into envScopes_
        il::ValueId frameEnv = il::kNoValue;  // the resume function's __env
        il::ValueId modeParam = il::kNoValue;
        il::ValueId sentParam = il::kNoValue;
        uint32_t stateSlot = 0;
        // Where the iteration record of a `yield*` in progress lives, or
        // `UINT32_MAX` in a body that has no delegation. In the FRAME, for the
        // reason every binding is: the delegation loop is re-entered from the
        // resume dispatch, and that edge defines no SSA value.
        //
        // ONE slot for a whole body, however many `yield*` sites it has. At
        // most one delegation can be in progress at a time: the loop is
        // straight-line control flow that is entered, driven to a completion
        // and left, and the only way to leave it and come back is its own
        // suspension — which comes back to the same delegation. `yield_lift`
        // is what makes that true of nested ones as well, hoisting the inner
        // `yield*` of `yield* (yield* g())` into a statement of its own.
        uint32_t iterSlot = UINT32_MAX;

        // An ASYNC function's machine: the same frame and resume function,
        // driven by the promise runtime instead of by a generator object.
        // One context type for both because every mechanism in it — the
        // resume blocks, the state slot, the abrupt-mode dispatch, the
        // cleanup routing through `finally` — is shared; what differs is the
        // tail (lower_async.cpp vs lowerGeneratorTail) and what a suspension
        // DOES (subscribe vs yield), and two context types would duplicate
        // every shared field to say so.
        bool isAsync = false;
        bool isAsyncGenerator = false;
        // Where the machine value (runtime/builtin_async.cpp's object) lives
        // in the frame, or UINT32_MAX in a generator. In the FRAME for the
        // reason every binding is: an `await` site needs it to subscribe the
        // resumption, and the site is re-entered from the resume dispatch,
        // whose edge defines no SSA value.
        uint32_t machineSlot = UINT32_MAX;

        // The frame slots reserved for the iteration records of `for-of` and
        // `for-in` loops whose body suspends — one per level of NESTING, in
        // depth order, because two such loops are only ever stepping at once
        // when one is inside the other. `activeIterLoops` is how deep lowering
        // currently is in them, and so which slot the next one claims.
        // Empty in a body that has no such loop, which is almost every body.
        std::vector<uint32_t> loopIterSlots;
        uint32_t activeIterLoops = 0;

        // How `next`, `return` and `throw` reach the body. Mirrors
        // GeneratorResumeMode in src/runtime/generator.h — which lowering
        // cannot include, because `bronze::lower` depends on `ast il support
        // types` and a module reaching into the runtime's headers would make
        // it depend on the runtime too. Here rather than in either unit that
        // reads them, because both do; pinned against the runtime by
        // tests/lower/lower_generator_test.cpp and by every oracle case that
        // drives a generator by hand. An async resumption uses `kModeNext`
        // and `kModeThrow` only — nothing external can `return` into an
        // async body.
        static constexpr double kModeNext = 0.0;
        static constexpr double kModeReturn = 1.0;
        static constexpr double kModeThrow = 2.0;
        // One per suspension point, in the order they were lowered, with the
        // body's first block at index 0. The entry block's dispatch is built
        // from this once the whole body is done, because how many there are is
        // not known until then.
        std::vector<il::BlockId> resumeBlocks;
    };
    std::optional<GeneratorContext> generator_;

    static const char* generatorStateSlotName();
    static const char* generatorEnvSlotName();
    static const char* generatorIterSlotName();
    static const char* asyncMachineSlotName();
    static std::string loopIterSlotName(uint32_t depth);
    Value emitConstF64(double value, il::Function& ilFn);
    Value emitIterResult(Value value, bool done, il::Function& ilFn);
    Value emitAsyncAwaitResult(il::Function& ilFn);
    Value emitFrameSlotGet(uint32_t slot, il::Function& ilFn);
    void emitFrameSlotSet(uint32_t slot, Value val, il::Function& ilFn);
    void emitGeneratorResult(Value value, bool done, il::Function& ilFn);
    void emitGeneratorFinish(Value value, il::Function& ilFn);
    void emitGeneratorDispatch(il::Function& ilFn);
    bool lowerGeneratorReturn(const ast::ReturnStmt* retStmt, il::Function& ilFn);
    std::optional<Value> lowerYield(const ast::YieldExpr& yield, il::Function& ilFn);
    // --- lower_yield_star.cpp: `yield*`, the delegation protocol ---------
    std::optional<Value> lowerYieldStar(const ast::YieldExpr& yield, il::Function& ilFn);
    // `isAsync` selects which machine the context describes; everything the
    // body itself needs — dispatch, resume blocks, cleanup routing — is one
    // code path either way.
    bool lowerResumeBody(const std::vector<const ast::Stmt*>& stmts, il::Function& resumeFn,
                         bool isAsync = false, bool isAsyncGenerator = false);
    bool lowerGeneratorTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);
    bool lowerAsyncGeneratorTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);

    // --- lower_async.cpp: the async driver over the same machine ----------
    // `await <v>`: subscribe the machine's resumption to v's settlement, then
    // suspend exactly as a yield does.
    std::optional<Value> lowerAwait(const ast::YieldExpr& await, il::Function& ilFn);
    std::optional<Value> lowerAwaitValue(Value awaited, Span span, il::Function& ilFn);
    // The async function's own body: park the state, build the resume
    // closure, hand it to the runtime driver, and return the promise the
    // driver made.
    bool lowerAsyncTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);

    // --- lower.cpp: module skeleton and function bodies ------------------
    // The one rule for a function-level environment record, shared by real
    // function bodies and by the module top level lowered as `main`.
    // `isGenerator` widens the record to the whole FRAME and adds the machine's
    // own two slots; see lower_generator.cpp.
    // `isAsync` rides beside `isGenerator` because an async body takes the
    // generator's whole-frame rule — an await is a suspension — plus one slot
    // of its own for the machine value.
    void enterFunctionEnv(const std::vector<ast::Param>& params,
                          const std::vector<const ast::Stmt*>& body, il::Function& ilFn,
                          bool isGenerator = false, bool isAsync = false);
    // The module scope, in two halves: its layout (before any body) and its
    // record (in `main`, which is lowered last). Splitting them is the whole
    // point — the layout is what a module function needs, the record is what
    // only the top level can create.
    void planModuleEnv(const std::vector<const ast::Stmt*>& topLevelStmts);
    void openModuleEnv(const std::vector<const ast::Stmt*>& topLevelStmts,
                       il::Function& mainFn);
    bool referencesModuleEnv(const std::vector<ast::Param>& params,
                             const std::vector<ast::StmtPtr>& body) const;
    bool lowerFunctionBody(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                           bool isGenerator = false, bool isAsync = false);
    bool lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn);

    // --- lower_unresolved.cpp: names that resolve to nothing ---
    bool resolvesName(const std::string& name) const;
    void warnUnresolved(const std::string& name, Span span);
    Value emitReferenceError(const std::string& name, Span span, il::Function& ilFn);
    // The names of the function expressions whose bodies are currently being
    // lowered, innermost last. A named function expression's own name is
    // DECLARED — 15.2.5 puts it in a scope of the function's own — and bronze
    // does not yet bind it. That is a known, named limitation, and this stack
    // is what keeps it a compile error: without it the name would fall off the
    // resolution ladder and become an unresolvable reference, turning a bug
    // bronze reports into a ReferenceError a program could catch.
    std::vector<std::string> namedFunctionExprs_;
    // Every name the function being lowered declares with `var`, at any block
    // depth. Same job as the stack above and for the same reason: 8.6.2 hoists
    // a `var` to the enclosing FUNCTION however deeply it is written, bronze
    // gives a slot only to the ones written at the top level, and the rest
    // would otherwise fall off the resolution ladder and be reported as
    // unresolvable globals — a compiler gap wearing a language error's costume,
    // which is precisely where the provable/unprovable line falls (bronze can
    // PROVE the name is declared, so it must refuse now).
    std::vector<std::string> functionVarNames_;
    // Which unresolved names have already been warned about. Per module, so
    // one `document` warning covers every mention of it.
    std::unordered_set<std::string> warnedUnresolved_;

    // --- lower_util.cpp: key constants, blocks, coercions, truthiness ----
    bool isProvidedGlobal(const std::string& name) const;
    uint32_t getKeyConstantIndex(const std::string& key);
    // The key constant a bracket index folds to, when the index is a literal
    // that names a property at compile time. `nullopt` means the site needs a
    // real elem.get / elem.set on the evaluated index.
    std::optional<uint32_t> literalIndexKey(const ast::Expr& index);
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

    // --- lower_scope.cpp: scopes, environments, closures -----
    bool declareVariable(const std::string& name, il::Type type, bool isConst, bool isLet,
                         bool isVar, bool isInitialized, il::ValueId valId, Span span);
    il::ValueId emitConstUndefined(il::Function& ilFn);
    il::ValueId emitEnvCreate(uint32_t slotCount, il::Function& ilFn);
    il::ValueId emitModuleEnvGet(il::Function& ilFn);
    void emitModuleEnvSet(il::ValueId env, il::Function& ilFn);
    Value emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn);
    void emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn,
                    bool assigning = false);
    // Is (depth, index) a slot holding a lexical binding, and so a read that
    // has to be checked? Asked by `emitEnvGet` alone, which is why every read
    // path — a local, a free variable of a closure, a compound assignment's
    // read half — gets the check without knowing it exists.
    bool envSlotIsLexical(uint32_t depth, uint32_t index) const;
    // The scope innermost right now takes its lexical bindings: each slot
    // marked, filled with the uninitialized marker, and BOUND under its name.
    // `scopeIndex` is the entry in `envScopes_` that owns them.
    void openLexicalBindings(size_t scopeIndex, const std::vector<std::string>& lexicalNames,
                             il::Function& ilFn);
    uint32_t envDepthOf(size_t scopeIndex) const;
    Value readBinding(const VarBinding& b, il::Function& ilFn);
    void writeBinding(VarBinding& b, Value val, il::Function& ilFn);
    bool findEnclosingEnvVar(const std::string& name, uint32_t& depth, uint32_t& index) const;
    void enterScope();
    // `extraDeclarations` names bindings the scope owns that its statement list
    // does not spell — a for-of head's, which is written outside the body but
    // belongs to it. A LIST because a destructuring head binds several. They
    // are deliberately NOT lexical for the dead zone's purposes: the loop binds
    // one before the body's first statement runs, every iteration, so it can
    // never be observed uninitialized.
    //
    // `extraLexicalDeclarations` is the other half of that distinction, and it
    // has exactly one caller: a `switch` body, whose lexical declarations are
    // written inside the clauses but belong to the CaseBlock's one scope
    // (14.12.2). They always get a slot, whether or not anything captures them,
    // because the dead zone is the only thing that makes them well defined.
    void enterScope(const std::vector<ast::StmtPtr>& stmts, il::Function& ilFn,
                    const std::vector<std::string>& extraDeclarations = {},
                    const std::vector<std::string>& extraLexicalDeclarations = {});
    void exitScope();
    // `site` is the AST node that IS the closure (a `FunctionExpr`, or a nested
    // `FunctionDecl` — a nested declaration desugars to a closure, so they are
    // one path). It is how inference is asked about a function with no module
    // index. `isArrow` decides one thing only: where `this` inside the body
    // comes from.
    //
    // `declaredName` and `jsName` are two different names and every caller has
    // to say both. The first is the IL's identifier, synthesized when the
    // source wrote none; the second is what 10.2.9 SetFunctionName makes
    // `f.name`, which for an anonymous function expression is "" unless the
    // surrounding syntax supplies one (8.6.2 NamedEvaluation), and for an
    // accessor is "get x" / "set x". `std::nullopt` means the name is not a
    // fact this compilation has — a member whose key is computed at runtime —
    // and reading `.name` off such a function is a diagnosed refusal.
    std::optional<Value> lowerClosure(const ast::Node& site, const std::string& declaredName,
                                      const std::optional<std::string>& jsName,
                                      const std::vector<ast::Param>& params,
                                      const std::string& returnTypeAnn,
                                      const std::vector<ast::StmtPtr>& body, Span span,
                                      il::Function& ilFn, bool isArrow = false);

    // ECMA-262 8.6.2 NamedEvaluation: an ANONYMOUS function expression in one
    // of the positions where the surrounding syntax names it — `const f = () =>
    // {}`, `f = function () {}`, `{ m: function () {} }`, a destructuring
    // default. Anything else is lowered as the ordinary expression it is.
    //
    // A parameter here rather than a `pendingName_` member, because a member
    // would leak: `const x = f(function () {});` would hand the argument the
    // binding's name, and nothing about the shape of the code would say so.
    std::optional<Value> lowerNamedEvaluation(const ast::Expr& expr, const std::string& name,
                                              il::Function& ilFn);

    // --- lower_pattern.cpp: binding patterns, defaults, spread -- How a
    // pattern's names reach their bindings. A declaration MAKES them and an
    // assignment writes ones that already exist, which is the only difference
    // between the two forms once the pattern itself is walked.
    struct PatternTarget {
        bool declare = true;
        bool isConst = false;
        bool isLet = true;
        bool isVar = false;
    };
    bool lowerPattern(const ast::BindingPattern& pattern, Value source,
                      const PatternTarget& target, il::Function& ilFn);
    bool lowerArrayPattern(const ast::BindingPattern& pattern, Value source,
                           const PatternTarget& target, il::Function& ilFn);
    bool lowerObjectPattern(const ast::BindingPattern& pattern, Value source,
                            const PatternTarget& target, il::Function& ilFn);
    bool bindPatternName(const std::string& name, Value value, const PatternTarget& target,
                         Span span, il::Function& ilFn);
    // A property reference used as a destructuring target, held open across the
    // element read. 13.15.5.2 evaluates the reference BEFORE the source element
    // it will receive, so `[o[i()]] = xs` calls `i` before the iterator steps —
    // which means the base and the key have to be lowered at one point and the
    // store emitted at another.
    struct PatternRef {
        Value object{il::kNoValue, il::Type::Dynamic};
        // Exactly one of these: a constant key index, or a computed key value.
        uint32_t keyIndex = 0;
        bool hasKeyIndex = false;
        Value index{il::kNoValue, il::Type::Dynamic};
    };
    std::optional<PatternRef> evalPatternRef(const ast::Expr& target, il::Function& ilFn);
    void storePatternRef(const PatternRef& ref, Value value, il::Function& ilFn);
    // `current === undefined ? <default>: current`, as a real branch rather
    // than a select: the default's side effects must happen only when it fires,
    // and only `undefined` fires it — `null` does not.
    //
    // `bindingName` is the SingleNameBinding this default belongs to, when it
    // is one: 14.3.3.3 and 15.1.3 both run their Initializer through 8.6.2
    // NamedEvaluation, so `const { a = () => {} } = o` and
    // `function f(a = () => {})` each give the function the name `a`. It is
    // empty when the target is a nested pattern instead, which is not a
    // BindingIdentifier and gets no name from anywhere.
    std::optional<Value> emitDefaultIfUndefined(Value current, const ast::Expr& defaultExpr,
                                                const std::string& bindingName,
                                                il::Function& ilFn);
    Value emitPatternCheck(Value source, bool isObject, il::Function& ilFn);
    std::optional<Value> lowerDestructuringAssign(const ast::DestructuringAssign* node,
                                                  il::Function& ilFn);
    // One parameter list, bound left to right into the function's own scope:
    // a default sees the parameters before it, so the order is the semantics
    // and not an implementation detail.
    bool lowerParamBindings(const std::vector<ast::Param>& params, uint32_t paramBase,
                            il::Function& ilFn);
    // The two facts about a parameter list that the CALLING CONVENTION needs
    // and the parameter types cannot carry: whether the last parameter
    // swallows the leftovers, and how few arguments a call may pass.
    static void applyParamShape(const std::vector<ast::Param>& params, il::Function& fn);
    static bool listHasSpread(const std::vector<ast::ExprPtr>& list);
    // Every element of `list` as one array, spreads expanded — the argument
    // vector of a call whose length is a runtime fact.
    std::optional<Value> lowerListToArray(const std::vector<ast::ExprPtr>& list,
                                          il::Function& ilFn);
    void emitContainerOp(il::Op op, Value container, Value value, il::Function& ilFn);

    // --- lower_stmt.cpp: statements --------------------------------------
    bool lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);
    bool lowerStmt(const ast::Stmt& stmt, il::Function& ilFn);
    bool lowerVarDecl(const ast::VarDecl* varDecl, il::Function& ilFn);

    // --- lower_class.cpp: classes, desugared -------
    bool lowerClassDecl(const ast::ClassDecl* cls, il::Function& ilFn);
    std::optional<Value> lowerClassExpr(const ast::ClassExpr* cls, il::Function& ilFn);
    std::optional<Value> lowerClass(const std::string& name, const std::string& superName,
                                    const std::vector<ast::ClassMethod>& methods, Span span,
                                    il::Function& ilFn);
    Value emitPrototypeOf(Value ctorVal, il::Function& ilFn);
    std::optional<Value> lowerSuperMember(const ast::SuperMember* sm, il::Function& ilFn);
    std::optional<Value> lowerSuperCall(const ast::SuperCall* sc, il::Function& ilFn);
    // The receiver of the function being lowered, wherever it comes from: a
    // parameter for an ordinary function, the environment for an arrow. `this`,
    // `super(...)` and `super.m()` all need the same answer, so they ask in the
    // same place.
    std::optional<Value> lowerThisValue(Span span, il::Function& ilFn);
    bool lowerReturnStmt(const ast::ReturnStmt* retStmt, il::Function& ilFn);

    // --- lower_control.cpp: control flow, block-argument SSA - One loop
    // variable and the type every block parameter standing for it takes —
    // header, exit, and the update/condition join alike, because the analysis
    // proves one type covering all of them.
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
    // One more parameter on a loop block, for the environment record rather
    // than for a variable. Appended AFTER `addLoopBlockParams` has run, so
    // that `collectEdgeArgs`'s positional match between `loopVars` and the
    // target's parameter list still holds for every edge.
    il::ValueId addEnvBlockParam(il::BlockId block, il::Function& ilFn);
    // Does ECMA-262 14.7.4.9 have anything to copy for this `for`? Only when
    // the head declared a lexical binding that lives in an environment record
    // AND a closure written under the loop reaches it. The second half is not
    // an optimisation: the copy is observable through a closure and through
    // nothing else, so a loop with none of them must keep the one-record shape
    // rather than allocate per iteration for a difference no program can see.
    bool forNeedsPerIterationEnv(const ast::ForStmt& forStmt) const;
    // 14.7.4.9 CreatePerIterationEnvironment itself: a record with the head
    // scope's layout hanging off the head scope's own parent — a SIBLING of
    // the record `source` is, never a child of it — with every head binding's
    // current value copied in.
    il::ValueId emitPerIterationEnv(il::ValueId source, uint32_t slotCount, il::ValueId parent,
                                    il::Function& ilFn);
    bool lowerIfStmt(const ast::IfStmt* ifStmt, il::Function& ilFn);
    bool lowerWhileStmt(const ast::WhileStmt* whileStmt, il::Function& ilFn);
    bool lowerDoWhileStmt(const ast::DoWhileStmt* doWhileStmt, il::Function& ilFn);
    bool lowerForStmt(const ast::ForStmt* forStmt, il::Function& ilFn);
    bool lowerBreakStmt(const ast::BreakStmt* breakStmt, il::Function& ilFn);
    // `break`/`continue` to `jumpStack_[targetIndex]`, running every `finally`
    // between here and there first.
    bool emitJumpCrossingFinallys(size_t targetIndex, bool toExit, il::Function& ilFn);
    bool lowerContinueStmt(const ast::ContinueStmt* continueStmt, il::Function& ilFn);
    // The label the statement now being lowered was written under, taken so
    // that no later statement can see it.
    std::string takePendingLabel();

    // --- lower_label.cpp: labelled statements and the jump-target stack ----
    bool lowerLabeledStmt(const ast::LabeledStmt* labeled, il::Function& ilFn);
    // A label on something that is not a loop or a switch — `lbl: { ... }` —
    // where the only jump the label admits is a `break` to the end.
    bool lowerLabeledBlock(const ast::LabeledStmt* labeled, il::Function& ilFn);
    // The entry a `break`/`continue` names, or null with the diagnostic
    // already reported. `forContinue` picks the iteration-statement rule.
    const JumpTarget* findJumpTarget(const std::string& label, bool forContinue, Span span);
    void emitJumpToTarget(const JumpTarget& target, il::BlockId block,
                          const std::vector<il::ValueId>& extraArgs, il::Function& ilFn);

    // --- lower_iter_loop.cpp: the two loops that walk a container ----------
    // for-of over the iterator, and for-in over the KEY SNAPSHOT the runtime
    // builds. One walk, because once the keys are an array the two loops differ
    // in nothing but what they open an iterator over.
    bool lowerForOfStmt(const ast::ForOfStmt* forOf, il::Function& ilFn);
    bool lowerForInStmt(const ast::ForInStmt* forIn, il::Function& ilFn);
    bool lowerIteratorLoop(const ast::Stmt& loopStmt, Value iterVal, const std::string& headName,
                           const ast::BindingPattern* headPattern, bool isConst, bool isLet,
                           bool isVar, const std::vector<ast::StmtPtr>& body,
                           il::Function& ilFn, bool isAwait = false);

    // --- lower_switch.cpp: selection and fallthrough -----------
    bool lowerSwitchStmt(const ast::SwitchStmt* sw, il::Function& ilFn);

    // --- lower_try.cpp: try/catch/finally and throw ------------
    bool lowerTryStmt(const ast::TryStmt* tryStmt, il::Function& ilFn);
    // `try { ... } catch (e) { ... }` with no finally, which is also the
    // protected region of a try/catch/finally: 14.15.3 defines the three-part
    // form as the two-part one wrapped in a finally, so there is one lowering
    // of each half rather than a third of the pair.
    bool lowerTryCatch(const ast::TryStmt* tryStmt, il::Function& ilFn);
    // The `try` BLOCK alone, in its own scope. Its own method because it is
    // lowered from two places: as the protected region of a try/catch, and
    // directly as the protected region of a try/finally with no catch.
    bool lowerTryBlock(const ast::TryStmt* tryStmt, il::Function& ilFn);
    bool lowerThrowStmt(const ast::ThrowStmt* throwStmt, il::Function& ilFn);
    // Runs the cleanups from the top of `cleanupStack_` down to `downTo`,
    // innermost first, each with the stack truncated below it so a jump
    // inside a finally does not re-run that finally. Stops early if one of
    // them completes abruptly — which is how `try { return 1 } finally
    // { return 2 }` produces 2 without a rule about precedence.
    bool runCleanups(size_t downTo, il::Function& ilFn);
    // The lowest `cleanupStack_` index a jump to `jumpStack_[targetIndex]`
    // has to run. `cleanupStack_.size()` when it crosses none.
    size_t cleanupDepthForJump(size_t targetIndex) const;
    // `iter.close %record, <suppress>`, the one instruction an
    // IteratorClose cleanup emits.
    void emitIterClose(il::ValueId record, bool suppress, il::Function& ilFn, bool isAsync = false);
    // One copy of a finally body, in its own scope. Lowered from the AST rather
    // than cloned: a re-lowering is fresh blocks and fresh SSA values, and
    // nothing in lowering is stateful across it.
    bool lowerFinallyBody(const ast::TryStmt& stmt, il::Function& ilFn);
    // Jumps into a fresh block stamped with `handler` and continues there.
    // What every copy of a finally body needs, and the reason a copy is not
    // simply emitted into whatever block lowering happens to be in.
    void openBlockUnderHandler(il::BlockId handler, il::Function& ilFn);

    // --- lower_expr_cond.cpp: conditional-expression joins ---
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

    // --- lower_expr_chain.cpp: optional chains ------ One short-circuit edge
    // out of a chain: where it leaves from, and what every binding held there.
    // The chain's join takes a parameter for the result and one per binding the
    // edges disagree about, so the edges have to be COLLECTED before the join's
    // parameters can be sized — which is why the jumps are emitted at the end
    // rather than as each link is lowered.
    struct ChainExit {
        size_t blockIdx = 0;
        il::ValueId result = il::kNoValue;  // kNoValue: this edge yields undefined
        VarStateMap state;
    };
    // What a SHORT-CIRCUITED chain produces. `undefined` for a read, which is
    // 13.3.9's answer — and `true` for `delete`, because 13.5.1.2 asks whether
    // the operand produced a Reference Record and a chain that stopped early
    // produced none.
    enum class ChainMiss { Undefined, True };
    std::optional<Value> lowerOptionalChain(const ast::Expr& expr, il::Function& ilFn);
    // The optional chain's n-way join around whatever `body` lowers. Two
    // callers, differing only in `miss`.
    std::optional<Value> lowerChainJoin(const std::function<std::optional<Value>()>& body,
                                        ChainMiss miss, il::Function& ilFn);
    // Lowers the base of a link, keeping it on the current chain's spine.
    std::optional<Value> lowerChainBase(const ast::Expr& base, il::Function& ilFn, bool onSpine);
    // `base === null || base === undefined ? <the whole chain's undefined> :
    // carry on`. Records the short-circuit edge and leaves the current block
    // at the continuation.
    void emitChainShortCircuit(Value base, il::Function& ilFn);
    // The short-circuit edges of the chain being lowered, empty otherwise.
    std::vector<ChainExit> chainExits_;
    // Set only while lowering the BASE of a chain link, and consumed by the
    // next `lowerExpr`.
    bool spinePos_ = false;

    // --- lower_expr.cpp: dispatcher, literals, identifiers, unary, assign --
    std::optional<Value> lowerExpr(const ast::Expr& expr, il::Function& ilFn);
    std::optional<Value> lowerAssignment(const ast::Binary* bin, il::Function& ilFn);

    // --- lower_update.cpp: `++`/`--` on each reference kind -----
    std::optional<Value> lowerUpdate(const ast::Unary& un, il::Function& ilFn);
    std::optional<Value> lowerMemberUpdate(const ast::MemberAccess& mem, ast::UnaryOp op,
                                           il::Function& ilFn);
    std::optional<Value> lowerIndexUpdate(const ast::IndexAccess& idx, ast::UnaryOp op,
                                          il::Function& ilFn);
    // The arithmetic half, shared so that the three reference kinds cannot
    // disagree about what ToNumeric produced.
    Value emitUpdateOld(Value oldVal, il::Function& ilFn);
    Value emitUpdateStep(Value oldNumeric, ast::UnaryOp op, il::Function& ilFn);

    // --- lower_expr_binary.cpp: the binary operator families ---
    std::optional<Value> lowerBinary(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerEquality(ast::BinaryOp op, Value lhs, Value rhs,
                                       il::Function& ilFn);
    // `<`, `>`, `<=`, `>=`. Which of the two algorithms ECMA-262 13.10.1 holds
    // is reached depends on the operand types, and the choice is not an
    // optimisation: an unproven operand may be a String, and step 3 compares
    // two of those by code unit without converting anything.
    std::optional<Value> lowerRelational(ast::BinaryOp op, Value lhs, Value rhs,
                                         il::Function& ilFn);
    // ECMA-262 ToInt32, and the bitwise/shift operators built on it. The int32
    // is an intermediate: every one of these produces an F64, because that is
    // the type the language gives their result and the only numeric element
    // inference has (see the definitions for why leaking I32 is unsound).
    Value emitToInt32(Value val, il::Function& ilFn);
    Value emitBitwise(il::Op op, Value lhs, Value rhs, il::Function& ilFn);
    Value emitPow(Value lhs, Value rhs, il::Function& ilFn);
    Value emitLogicalNot(Value boolVal, il::Function& ilFn);
    static std::optional<il::Op> bitwiseOpFor(ast::BinaryOp op);

    // --- lower_object.cpp: objects, property access, new, calls
    std::optional<Value> lowerObjectLit(const ast::ObjectLit* objLit, il::Function& ilFn);
    // `delete <unary>`. Dispatches on the OPERAND's node kind rather than
    // lowering it, because delete never reads the property it names.
    std::optional<Value> lowerDelete(const ast::Unary& del, il::Function& ilFn);
    std::optional<Value> lowerDeleteReference(const ast::Unary& del, il::Function& ilFn);
    // `get k() {}` / `set k(v) {}` on `target`, from an object literal or a
    // class body; `enumerable` is the only thing that differs between them.
    bool emitAccessorDef(Value target, const std::string& key, ast::AccessorKind kind,
                         const ast::FunctionExpr& fn, bool enumerable, il::Function& ilFn);
    bool emitAccessorDefComputed(Value target, Value key, ast::AccessorKind kind,
                                 const ast::FunctionExpr& fn, bool enumerable, il::Function& ilFn);
    std::optional<Value> lowerArrayLit(const ast::ArrayLit* arrLit, il::Function& ilFn);
    std::optional<Value> lowerNewExpr(const ast::NewExpr* newExpr, il::Function& ilFn);
    // `onSpine` says this node is a link of an optional chain already being
    // lowered, which decides one thing only: whether its BASE continues the
    // same chain.
    std::optional<Value> lowerMemberAccess(const ast::MemberAccess* mem, il::Function& ilFn,
                                           bool onSpine = false);
    std::optional<Value> lowerIndexAccess(const ast::IndexAccess* idxAccess, il::Function& ilFn,
                                          bool onSpine = false);
    // The READ half of `o[k]`, with the base already lowered, boxed and
    // short-circuited. Its own step because a CALL through `o[k]()` needs the
    // base twice — once as the callee's object and once as the receiver — and
    // lowering the base a second time would evaluate it twice (ECMA-262
    // 13.3.6.1 evaluates the MemberExpression once and passes it as the this
    // value).
    std::optional<Value> emitIndexRead(const ast::IndexAccess& idxAccess, Value objBoxed,
                                       il::Function& ilFn);
    std::optional<Value> lowerCall(const ast::Call* call, il::Function& ilFn,
                                   bool onSpine = false);
};

}  // namespace bronze::lower
