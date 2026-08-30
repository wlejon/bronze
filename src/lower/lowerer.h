#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "lower/bigint_reach.h"
#include "ast/clone.h"
#include "il/il.h"
#include "lower/infer_stats.h"
#include "lower/lowerer_state.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/pins.h"
#include "types/result.h"

namespace bronze::lower {

// Operators whose EVERY operand goes through ToNumeric on every branch. One
// table, two consumers: the typed-element seam (lower_typed_elem.cpp, which
// defines it) and the env-slot number proof (lower_scope.cpp).
bool alwaysCoercingBinary(ast::BinaryOp op);

// The AST -> IL pass. One instance per module; its methods are defined
// across the lower_*.cpp units named in the group comments below, each of
// which is one seam of the design it implements.
class Lowerer {
public:
    using Value = lower::LowererValue;
    using VarBinding = lower::VarBinding;
    using JumpKind = lower::JumpKind;
    using JumpTarget = lower::JumpTarget;
    using CleanupKind = lower::CleanupKind;
    using CleanupFrame = lower::CleanupFrame;
    using EnvScopeInfo = lower::EnvScopeInfo;
    using VarState = lower::VarState;
    using VarStateMap = lower::VarStateMap;
    using ExprJoin = lower::ExprJoin;
    using StaticSlotSite = lower::StaticSlotSite;
    using GeneratorContext = lower::GeneratorContext;

    // `inference` may be null: that is the no-inference mode, and it
    // reproduces the pre-inference calling convention exactly (see lower.h).
    // `hostGlobals` may be null too — no manifest — and is copied into a set
    // here because isProvidedGlobal is asked once per free-identifier mention.
    Lowerer(const ast::Module& astModule, DiagnosticSink& diags,
            const types::InferenceResult* inference,
            const std::vector<std::string>* hostGlobals = nullptr,
            const SourceSet* sources = nullptr,
            InferStatsCollector* stats = nullptr,
            bool assumeNoBigInt = false,
            const types::PinManifest* pins = nullptr,
            const std::string& censusOutPath = {})
        : astModule_(astModule), diags_(diags), inference_(inference),
          pins_(pins), sources_(sources), stats_(stats) {
        // Assigned rather than initialized: the member sits with the rest of
        // the census machinery, far below the four the list already names, and
        // an initializer list out of declaration order is a warning this build
        // treats as an error.
        censusOutPath_ = censusOutPath;
        if (hostGlobals) hostGlobals_.insert(hostGlobals->begin(), hostGlobals->end());
        if (stats_ && sources_) stats_->setSourceSet(sources_);
        typedElemDisabled_ = typedElemSeamDisabled() ||
                             hostGlobals_.count("Float64Array") != 0 ||
                             hostGlobals_.count("Float32Array") != 0;
        staticShapesDisabled_ = staticShapeSeamDisabled();
        familyGuardDisabled_ = familyGuardSeamDisabled();
        unboxedFieldsDisabled_ = unboxedFieldSeamDisabled();
        // Both halves: the claim about channels the compiler cannot see (a
        // promise only where there ARE any — driver.cpp), AND the text's scan.
        numericArithDisabled_ =
            numericArithSeamDisabled() || !assumeNoBigInt ||
            bigIntMayReach(astModule, hostGlobals ? *hostGlobals
                                                  : std::vector<std::string>{});
    }

    std::optional<il::Module> lower();

private:
    const ast::Module& astModule_;
    DiagnosticSink& diags_;
    // Never dereferenced outside lower_infer.cpp: every other unit asks the
    // accessors there, which answer "unproven" when this is null.
    const types::InferenceResult* inference_ = nullptr;
    // The `--pins` manifest, or null. Lowering reads exactly one part of it —
    // the env-slot entries — because an env slot is a LOWERING object: it has
    // no source name inference can key on, only a (function, binding) pair and
    // a record layout this pass invents. The field pins are inference's and
    // reach here as `provenFieldReads` / `nullishNumberFieldReads`.
    const types::PinManifest* pins_ = nullptr;
    il::Module ilModule_;
    // Names a `--host-globals` manifest admitted: the open half of the
    // provided-globals set. isProvidedGlobal consults it after the builtin
    // list, so a read of one lowers to `global.get` like a builtin's does.
    std::unordered_set<std::string> hostGlobals_;
    std::unordered_map<std::string, uint32_t> functionIndices_;
    std::unordered_map<uint32_t, Value> functionRefMap_;
    std::unordered_map<std::string, uint32_t> keyConstants_;
    // keyConstants_ read the other way, filled as indices are handed out.
    std::vector<std::string> keyStrings_;
    uint32_t icSiteCounter_ = 0;

    // --- the direct method-call edge (il.h `directTarget`) ----------------
    // Naming a method-call site's callee is a two-sided fact and the two sides
    // are lowered in the wrong order: a top-level function's body is lowered
    // BEFORE `main`, and a class body is evaluated INSIDE `main`. So
    // `c.multiplyMatrices(a, b)` in `run` is lowered while
    // `Matrix4.prototype.multiplyMatrices` is not yet a function in the module
    // at all. Both sides therefore record what they know, keyed on names, and
    // `resolveDirectMethodTargets` matches them once the module is whole.
    //
    // Keyed on the site's IC INDEX rather than on a (function, block,
    // instruction) triple: the index is already unique across the module and
    // already on the instruction, so the resolver needs no second numbering to
    // stay in step with.
    struct MethodCallSite {
        // The class lowering believes the receiver has, or empty. The nearest
        // declaration at or above it is the callee; a subclass override below
        // it is exactly what the backend's code-pointer compare rejects.
        std::string receiverClass;
        std::string method;
    };
    std::unordered_map<uint32_t, MethodCallSite> methodCallSites_;
    // class name -> method name -> module function index, for the ordinary
    // instance methods (not static, not private, not an accessor, and not a
    // computed key: none of those is reachable as `recv.<name>`).
    std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> classMethods_;
    // class name -> the name it extends, so the resolver can walk to the
    // nearest declaration without asking inference a second time.
    std::unordered_map<std::string, std::string> classSuper_;
    // The module function `lowerClosure` last appended. Valid only immediately
    // after a successful call; `lowerClass` is the one reader.
    uint32_t lastClosureFnIndex_ = il::Instruction::kNoDirectTarget;
    void recordMethodCallSite(const il::Instruction& inst, const ast::Expr& receiver,
                              const std::string& method);
    void recordClassMethod(const std::string& className, const std::string& superName,
                           const std::string& method, uint32_t fnIndex);
    void recordClassSuper(const std::string& className, const std::string& superName);
    void resolveDirectMethodTargets();

    // One per site that took the static-slot form. Separate from
    // `icSiteCounter_` so the cell array is proportional to what actually
    // proved, not to every property site in the program.
    uint32_t staticSiteCounter_ = 0;
    // One per TAGGED TEMPLATE in the module: 13.2.8.4 keeps a template
    // object per site, so each site owns a cache cell in the module's own
    // table and this is what numbers them.
    uint32_t templateSiteCounter_ = 0;

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
    std::vector<CleanupFrame> cleanupStack_;

    // The label a `label:` just read, waiting for the loop or switch it
    // fronts to claim it. Cleared by whichever statement lowering reaches
    // next, so a label can never leak onto a second statement.
    std::string pendingLabel_;
    size_t currentBlockIdx_ = 0;

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
    // Whether the module top level lowers as SEGMENTS — a chain of
    // `main.seg<K>` functions `main` calls in order — instead of one body.
    // Decided from the top level's source size before planModuleEnv runs,
    // because the plan is what makes segmentation sound: with every top-level
    // binding in the module record, no SSA value crosses a statement
    // boundary, so any boundary is a safe cut. The point is parallel object
    // emission (llvm_backend.cpp): a function cannot be partitioned, and a
    // bundle's top level was 6–10% of the whole module in ONE function.
    bool segmentTopLevel_ = false;

    // --- Conditional-expression joins -----------------------------------
    // &&, ||, ?? and ternary evaluate an operand on only some paths, so a
    // variable assigned inside such an operand needs a join parameter,
    // exactly like an if-statement arm. States are value snapshots because
    // assignments rebind varBindings_ entries in place.

    // --- lower_infer.cpp: what inference proved -------------- The single
    // place "there is no inference result" is answered, so no other unit tests
    // inference_ and --no-infer stays one null pointer rather than a flag
    // threaded through every site.
    static il::Type ilTypeOf(types::Type t);

    // The node inference actually walked, for a node lowering is holding.
    //
    // The two are the same node everywhere but one: a class constructor's body
    // is COPIED here, so that the field initializers can be spliced into it
    // without editing the tree inference read. Every proof is keyed on node
    // identity, so without this translation a copy answers "unproven" to every
    // question and each of three.js's 200 constructors lowers as if inference
    // had never run — 2 088 property sites, every loop counter's f64 block
    // parameter, and every certified field read among them.
    //
    // The copies that carry origins are exactly the ones evaluated where the
    // original was; ast/clone.h states the rule and names the copy that is
    // deliberately left without them.
    const ast::Node* inferenceNode(const ast::Node& n) const;
    const ast::Expr& inferenceExpr(const ast::Expr& e) const;
    const ast::Stmt& inferenceStmt(const ast::Stmt& s) const;
    // Innermost last. A class declared inside another class's constructor is a
    // copy of a copy, so the translation is a chain and this is its depth.
    std::vector<const ast::CloneOrigins*> cloneOrigins_;

    types::Type inferredType(const ast::Expr& expr) const;
    bool provenNumber(const ast::Expr& expr) const;
    bool monomorphicPropSite(const ast::Expr& receiver) const;
    // Whether this receiver can be a function object, so the backend knows to
    // emit the statics arm here and nowhere else (`il::Instruction::icFnRecv`).
    bool functionBindingReceiver(const ast::Expr& receiver, uint32_t keyIndex) const;
    // Did inference prove this call reaches a pristine builtin Math method?
    // (module-wide taint scan clean, name unshadowed at the site)
    bool pristineMathCall(const ast::Expr& call) const;

    // --- lower_typed_elem.cpp: proven typed-array element access ---------
    // `v[i]` where inference proved `v` a Float64Array/Float32Array view and
    // `i` a number lowers to elem.get.typed / elem.set.typed — an unboxed f64
    // read or write with the index and bounds checks inline and no receiver
    // guards. The get yields NaN where the language yields `undefined`
    // (invalid or out-of-bounds index), which is exact under ToNumber and
    // nothing else, so every consumer below either proves a coercion or keeps
    // the dynamic path.
    //
    // The element kind travels as the raw types::TypedArrayElem number.
    static bool typedElemSeamDisabled();  // BRONZE_NO_TYPED_ELEM, read once
    std::optional<uint32_t> typedElemAccessKind(const ast::Expr& e) const;
    bool provenArrayOrTypedArray(const ast::Expr& e) const;
    bool binaryCoercesOperand(ast::BinaryOp op, const ast::Expr& other) const;
    bool typedElemCompoundAdmissible(ast::BinaryOp op, const ast::Expr& rhs) const;
    std::optional<Value> lowerCoercingOperand(const ast::Expr& e, il::Function& ilFn);
    std::optional<Value> lowerTypedElemRead(const ast::IndexAccess& idx, uint32_t elemKind,
                                            il::Function& ilFn);
    std::optional<Value> lowerTypedElemAssign(const ast::Binary* bin,
                                              const ast::IndexAccess& idxAccess,
                                              uint32_t elemKind, il::Function& ilFn);
    bool definitelyNumericOperand(const ast::Expr& e, int depth) const;
    bool typedElemBindingUsesCoerce(const std::string& name, const ast::VarDecl* self) const;
    void emitTypedElemSet(Value objBoxed, Value idxF64, Value valF64, uint32_t elemKind,
                          il::Function& ilFn);
    // The statement list of the function whose body is being lowered, for the
    // binding-use scan; null outside a body (params, module segments).
    const std::vector<ast::StmtPtr>* currentBodyStmts_ = nullptr;
    // BRONZE_NO_TYPED_ELEM=1 or a --host-globals manifest naming either
    // constructor (the host's Float64Array is not the builtin the proof
    // named). A compile-time seam like --no-infer, and like it, an A/B
    // bisection tool, not a runtime toggle.
    bool typedElemDisabled_ = false;
    // BRONZE_NO_STATIC_SHAPES=1. Compile-time, exactly like the seam above and
    // for the same reason: the mechanism it gates is a shape of EMITTED CODE,
    // and a runtime toggle would have to be a load and a branch on the fast
    // path it exists to measure. The class-layout ANALYSIS still runs under it
    // — `--infer-stats` reports the same layouts and the same refusals — so
    // what an A/B isolates is the code, not the proof.
    bool staticShapesDisabled_ = false;
    static bool staticShapeSeamDisabled();
    // BRONZE_NO_FAMILY_GUARD=1. The family guard alone, so an A/B can separate
    // what chunk 6's identity cells bought from what recognising a whole
    // `extends` subtree per site buys on top. With it on, a `this` receiver of
    // an extended class declines its claim exactly as it did before — the
    // 952-site refusal chunk 6 shipped.
    bool familyGuardDisabled_ = false;
    static bool familyGuardSeamDisabled();
    // BRONZE_NO_UNBOXED_FIELDS=1. The audited field's raw load and the native
    // arithmetic over it, so an A/B can separate what the write AUDIT costs
    // (correctness, always on — there is no seam for it) from what CASHING the
    // proof buys. With it on, a proven field read is the boxed value it always
    // was and its conversion is the checked unbox.
    bool unboxedFieldsDisabled_ = false;
    static bool unboxedFieldSeamDisabled();
    // What `planClosureParamNumbers` proved for ONE function body, keyed by the
    // declaration node. A nested declaration appears in exactly one enclosing
    // statement list, so a frame holds one entry per function.
    using ProvenParamPlan = std::unordered_map<const ast::FunctionDecl*, std::vector<bool>>;
    // One frame per function body being lowered, innermost last, opened and
    // closed by `lowerFunctionBody`.
    //
    // The frame CANNOT outlive its body, because the key is a node ADDRESS and a
    // body's nodes are not always the module tree's: a class constructor is
    // lowered from a copy that dies with `lowerClass` (the `cloneOrigins_` note
    // says why the copy exists). An entry left behind by a dead copy is answered
    // for whatever node the allocator puts at that address next, which types a
    // parameter f64 on the evidence of call sites in an unrelated function — and
    // only on the runs where the addresses happen to collide, so the compiler
    // stops being a function of its input.
    std::vector<ProvenParamPlan> provenClosureParams_;
    // BRONZE_NO_NUMERIC_ARITH=1, a BigInt in reach, or a host boundary no
    // `assumeNoBigInt` covers. Gates ONE decision in `lowerBinary`: whether
    // `*`, `-`, `/`, `%` over a boxed operand produce a boxed result or an f64
    // one. The boxed result is what earns a GC root slot, and a rooted value is
    // stored and reloaded around every instruction it survives — so this seam
    // is the difference between three.js's matrix math keeping its
    // intermediates in registers and spilling all of them.
    //
    // The refusal is the whole-program BigInt scan, not a per-site test: with
    // one in reach, 13.15.3's BigInt algorithm is live and the result of `*`
    // genuinely can be a heap value.
    bool numericArithDisabled_ = false;
    static bool numericArithSeamDisabled();
    // A read of an audited field on a receiver watched being made: the one
    // licence for the raw unbox.
    bool provenFieldRead(const ast::Expr& e) const;
    Value emitRawUnbox(Value boxed, il::Function& ilFn);
    // A read of a `--pins number-or-nullish` field: the licence for the
    // compare-and-two-constants form of ToNumber, and for that alone.
    bool nullishNumberFieldRead(const ast::Expr& e) const;
    Value emitNullishUnbox(Value boxed, il::Function& ilFn);
    // The module's class-layout table, in family preorder, filled once the
    // first family site is claimed (so a program that proves nothing emits
    // nothing). Interning the field names is what makes this a lowering step
    // rather than a codegen one: the names have to become key-pool indices.
    void buildClassFamilyTable();
    bool classFamilyTableBuilt_ = false;

    // The `--pins number` field NAMES this program declares, interned into the
    // module's key pool and handed to the runtime at init (il.h,
    // `slotReprFields`). Built once, at the end of lowering, from the same
    // (class, field) lookup a pin barrier resolves — so a name reaches the list
    // exactly when a read of it would have spent the pin unchecked.
    void buildSlotReprTable();
    // The instance slot a proven class layout puts `key` at on `receiver`, and
    // the module-global cell index the site gets, or nullopt. Allocates the
    // cell, so it is called exactly once per site.
    std::optional<StaticSlotSite> claimStaticSlot(const ast::Expr& receiver,
                                                  const std::string& key, bool forWrite);
    // Stamps a PropGet/PropSet whose `keyIndex` is already set. The key is read
    // back out of the module's constant table rather than passed, so the
    // fourteen call sites all say the same short thing and none of them can
    // stamp a slot for a key the instruction is not actually using.
    void stampStaticSlot(il::Instruction& inst, const ast::Expr& receiver);
    // The class-layout verdicts, forwarded to --infer-stats once per module.
    void reportClassLayouts();
    il::Type mergeParamType(const ast::Stmt& mergePoint, const std::string& name) const;
    const types::Signature* provenSignature(uint32_t moduleFnIndex) const;
    types::Type provenParamType(uint32_t moduleFnIndex, size_t paramIndex) const;
    types::Type provenReturnType(uint32_t moduleFnIndex) const;
    types::Type provenClosureReturn(const ast::Node& site) const;
    bool applyProvenSignature(const ast::FunctionDecl& fnDecl, uint32_t moduleFnIndex,
                              il::Function& fn);
    // The `--pins` signature entries for `fn.name`, applied after every proof
    // has spoken. Only ever moves a slot from Dynamic to F64.
    bool applySignaturePins(const std::vector<ast::Param>& params, Span span, il::Function& fn);
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
    bool lowerTopLevelSegments(const std::vector<const ast::Stmt*>& topLevelStmts,
                               il::Function& mainFn);
    bool referencesModuleEnv(const std::vector<ast::Param>& params,
                             const std::vector<ast::StmtPtr>& body) const;
    bool lowerFunctionBody(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                           bool isGenerator = false, bool isAsync = false);
    bool lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn);
    // The body proper. Reached only through `lowerFunctionBody`, which is the
    // one place that opens and closes this body's closure-parameter plan frame,
    // so that no early `return false` in here can leave the frame behind.
    bool lowerBodyWithPlan(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                           bool isGenerator, bool isAsync);

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
    // The `file:` URL of one module, for `import.meta.url`.
    std::string moduleUrl(uint16_t fileId) const;
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
    // Whether the lexical slot's initializer is proven to run before any user
    // code can, so its dead zone is unreachable (lowerer_state.h,
    // `slotIsDefiniteInit`).
    bool envSlotDefiniteInit(uint32_t depth, uint32_t index) const;
    // BRONZE_NO_DEFINITE_INIT=1, the A/B seam for the analysis above. Asked at
    // the two places that MARK a slot, so the marker and the check agree.
    static bool definiteInitDisabled();
    // The same question for 9.1.1.1.3's immutable bindings, asked by
    // `emitEnvSet` alone — see EnvScopeInfo::slotIsImmutable.
    SlotImmutability envSlotImmutability(uint32_t depth, uint32_t index) const;

    // Does this slot hold a Number at every read? Asked by `emitEnvGet` alone,
    // which is what makes the answer a calling convention for the binding
    // rather than a per-site opinion.
    bool envSlotIsF64(uint32_t depth, uint32_t index) const;
    // Decides that for a function's own environment record, once, at the point
    // the record's layout is fixed. See lower_scope.cpp for the rule.
    void planEnvSlotNumberTypes(const std::vector<ast::Param>& params,
                                const std::vector<const ast::Stmt*>& body,
                                const std::string& functionName, EnvScopeInfo& info) const;
    void emitImmutableAssign(const std::string& name, il::Function& ilFn);

    // --- the `--pins` write barriers (types/pins.h, stage B1) ---------------
    //
    // One emitter, five callers, and one rule between them: the barrier goes
    // where a claim can be CONTRADICTED, never where it is spent. `val` is the
    // value about to be written, BEFORE boxing, so the caller's static type is
    // still on it — an `F64` needs no barrier at all, which is what makes the
    // no-violation tax nothing on a program whose promises are kept.
    //
    // Answers whether a guard was actually EMITTED, which is the caller's
    // licence to treat the value as the pinned shape from here on: the guard
    // throws on a violation, so the code after it is the non-violating path
    // and nothing else can reach it.
    bool emitPinGuard(Value val, const std::string& pinText, il::PinBarrier kind,
                      il::Function& ilFn);
    // Does `val` already satisfy `kind` by the IL type it carries? An f64 IL
    // value is a Number by construction, which covers arithmetic, a pinned
    // read, a typed parameter and a typed call result — and is why the pinned
    // kernels emit no barriers in their loops.
    static bool pinSatisfiedStatically(Value val, il::PinBarrier kind);
    // The pin declared for `receiver.<key>`, resolved the way the flow
    // analyzer resolves it for the READ (types/flow_expr.cpp `pinnedField`),
    // so that the barrier and the claim can never disagree about which entry
    // is in force. Null when nothing is pinned there.
    const types::PinKind* pinnedFieldAt(const ast::Expr& receiver, const std::string& key,
                                        std::string* pinTextOut) const;
    // The barrier for a store to a pinned FIELD, emitted before the `prop.set`
    // that carries it. `key` is the property as the source spells it.
    void emitPinFieldBarrier(const ast::Expr& receiver, const std::string& key, Value val,
                             il::Function& ilFn);
    // The barrier for a store to an ELEMENT of a `numeric-elements` array. A
    // no-op for every other element kind, because every other one is a proven
    // view whose store converts rather than reinterprets.
    // Answers whether a guard was emitted, for the same reason `emitPinGuard`
    // does: a store whose value the barrier just proved a Number may take the
    // array's RAW element form instead of the dynamic ladder.
    bool emitPinnedElementBarrier(uint32_t elemKind, Value val, il::Function& ilFn);

    // --- the PIN CENSUS (`--census`, src/runtime/pin_census.h, stage C1) -----
    //
    // The mirror image of the barriers above. A barrier is emitted where a
    // manifest HAS made a claim; a census record is emitted where a manifest
    // COULD make one and nothing in the compiler can — an env slot the fixpoint
    // refused, a parameter no proof typed, a return the convention left
    // Dynamic, a store to a field whose contents are unknown. Everything the
    // proofs own is silently absent, which is what stage E4's HANDOFF (c) asked
    // for: the census must not spend its budget on what is proved for free.
    bool censusEnabled() const { return !censusOutPath_.empty(); }
    // An IL function's name as a MANIFEST spells it: the module linker's
    // `modN.` prefix dropped, everything else kept. Shared by the barriers
    // (which name the line a violation broke) and by the census (which writes
    // that line), because two spellings would be two different entries and the
    // loop from a thrown TypeError back to the file that caused it would not
    // close (stage B1's HANDOFF (d), item 2).
    static std::string manifestOwnerName(const std::string& ilName);
    std::string censusOutPath_;
    // One census site: an instruction observing `val`, and a row in the
    // module's site table so that the site is known even if it never runs.
    void emitCensusRecord(Value val, const std::string& target, il::CensusSite kind,
                          il::Function& ilFn);
    // A row with NO instruction: the site table alone. Used for the refusals
    // that need no observation — an owner spelling that would govern two
    // different IL functions.
    void addCensusSite(const std::string& target, il::CensusSite kind, bool refuses);
    // A store to `receiver.<key>` under `--census`: one `Field` site naming the
    // receiver's class when inference can name it, one `OpaqueFieldStore` site
    // naming the bare field when it cannot. The second is B1's negative 1
    // turned into data — it is what marks an entry `@observed`.
    void emitCensusFieldRecord(const ast::Expr& receiver, const std::string& key, Value val,
                               il::Function& ilFn);
    // The class name a field entry would be written against, or empty. Resolved
    // the way `pinnedFieldAt` resolves it and for the same reason. `opaque` is
    // set when the receiver is one B1's barrier could not hold — inference
    // types it `dynamic`, or as an object of no known shape class.
    std::string censusFieldOwner(const ast::Expr& receiver, bool* opaque) const;
    // A `param` / `return` entry's owner as the manifest spells it, and whether
    // that spelling is UNAMBIGUOUS across the module's IL functions. An entry
    // matches an IL name by suffix (types/pins.cpp `forEachSpelling`), so a
    // bare `clamp` written for a module function would also govern a
    // `Bar.clamp` elsewhere — and that one may be a shape the pin cannot be
    // honoured on at all, which is a hard error rather than a wrong number. The
    // ambiguous spellings are refused by a table row.
    void refuseAmbiguousCensusOwners();
    // Sites created for `param`/`return`/`function` entries, by owner spelling,
    // so the pass above can find the ambiguous ones. Filled as sites are made.
    std::vector<std::pair<std::string, il::CensusSite>> censusSignatureOwners_;
    // The scope innermost right now takes its lexical bindings: each slot
    // marked, filled with the uninitialized marker, and BOUND under its name.
    // `scopeIndex` is the entry in `envScopes_` that owns them.
    void openLexicalBindings(size_t scopeIndex, const std::vector<std::string>& lexicalNames,
                             const std::vector<std::string>& definiteNames,
                             const std::vector<std::string>& constNames,
                             il::Function& ilFn);
    uint32_t envDepthOf(size_t scopeIndex) const;
    Value readBinding(const VarBinding& b, il::Function& ilFn);
    void writeBinding(VarBinding& b, Value val, il::Function& ilFn);
    // 9.1.1.1.5 step 7 for a `const` that never became an environment slot,
    // which is every `const` no closure reads: its value lives in SSA, so
    // `emitEnvSet`'s arm never sees it and an assignment was a rename. True
    // when the store must not happen, with the TypeError already emitted.
    bool refuseConstAssignment(const VarBinding& b, il::Function& ilFn);
    bool findEnclosingEnvVar(const std::string& name, uint32_t& depth, uint32_t& index) const;

    // --- the static call plan (lowerer_state.h, EnvScopeInfo::slotIsStableFn) --
    //
    // Marks every slot of `info` whose binding is a function declaration of
    // this scope that nothing in the scope's whole lexical reach can reassign.
    // `params` is the parameter list whose defaults are code of this scope too,
    // or null for a scope that has none (a block, a switch's CaseBlock).
    void planStableFunctionSlots(const std::vector<const ast::Stmt*>& stmts,
                                 const std::vector<ast::Param>* params, EnvScopeInfo& info) const;
    void planStableFunctionSlots(const std::vector<ast::StmtPtr>& stmts,
                                 const std::vector<ast::Param>* params, EnvScopeInfo& info) const;
    // The other half: the hoisting pass says which IL function the closure it
    // just made for `name` is, once, at the point the binding is created.
    void recordStableFunctionSlot(size_t scopeIndex, uint32_t slot, uint32_t fnIndex);

    // --- the closure PARAMETER proof (lower_scope.cpp) -----------------------
    //
    // For every plain `function f(...)` declared directly in `stmts`, decides
    // which of its parameters are Numbers at every call. The rule and its
    // refusals are in lower_scope.cpp; the result fills the frame
    // `lowerFunctionBody` opened for `stmts`, and is consumed from there by
    // `applyProvenClosureParams` when the declaration is lowered.
    void planClosureParamNumbers(const std::vector<ast::Param>& params,
                                 const std::vector<ast::StmtPtr>& stmts);
    // Gives one nested declaration's IL skeleton the f64 parameter slots the
    // plan above proved for it. Runs beside `applySignaturePins` and only ever
    // moves a slot from Dynamic to F64, so a pin that says the same thing is a
    // no-op either way round.
    void applyProvenClosureParams(const ast::Node& site, const std::vector<ast::Param>& params,
                                  il::Function& fn) const;
    // BRONZE_NO_CLOSURE_PARAM_PROOF=1, the A/B seam for it.
    static bool closureParamProofDisabled();
    // Does `name` denote a function this module lowered, in a binding nothing
    // can rebind? On yes, `envHops` is how many parent links a caller here
    // walks to reach the record that closure captured, and `fnIndex` names it.
    bool findStableFunctionCallee(const std::string& name, uint32_t& envHops,
                                  uint32_t& fnIndex) const;
    // The one place a `call @F(...)` is built. `envBase` is the caller's own
    // environment record and `envHops` the number of parent links from it to
    // the record the callee captured — both `kNoValue`/0 for a callee that
    // needs no environment at all. Answers nullopt only on a lowering failure;
    // whether the call CAN take this shape is `directCallShapeFits`, asked by
    // the caller before this is reached.
    std::optional<Value> lowerDirectCall(const ast::Call* call, uint32_t calleeIdx,
                                         il::ValueId envBase, uint32_t envHops,
                                         il::Function& ilFn);

    void enterScope();
    // A scope whose slots no declaration spells: the class-evaluation record
    // that holds a class body's private names. Everything `enterScope` does to
    // create a record — the generator's downward child link included — with the
    // slot list given rather than derived from statements. Undone by
    // `exitScope`, which is why it keeps that function's invariants.
    void pushSyntheticEnv(std::vector<std::string> slots, il::Function& ilFn);
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
                                      il::Function& ilFn, bool isArrow = false,
                                      bool bindsOwnName = false);

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
        // `({ a: this.#x } = v)` — a PRIVATE member as the target. Not a key of
        // any kind: the store is a private-element write, so the node that
        // names the element is carried here and `keyIndex` means nothing. Held
        // as a pointer to the AST rather than a resolved element because the
        // kind dispatch (field / method / accessor) belongs to one place, and
        // that place is `lowerPrivateWrite`.
        const ast::MemberAccess* privateTarget = nullptr;
        // The RECEIVER as the source wrote it, kept so that a destructuring
        // store into a pinned field can ask the same question an ordinary
        // assignment asks (lower_pin.cpp). Null when the target is private.
        const ast::Expr* receiverExpr = nullptr;
        // The property as the source spells it, for the same reason. Empty
        // where the key is computed, which no pin can name.
        std::string keyName;
    };
    std::optional<PatternRef> evalPatternRef(const ast::Expr& target, il::Function& ilFn);
    bool storePatternRef(const PatternRef& ref, Value value, il::Function& ilFn);
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
    std::optional<Value> lowerClass(const std::string& name, const ast::Expr* superClass,
                                    const std::string& superName,
                                    const std::vector<ast::ClassMethod>& methods, Span span,
                                    il::Function& ilFn);
    Value emitPrototypeOf(Value ctorVal, il::Function& ilFn);

    // --- lower_private.cpp: private class elements ------------------------
    // How one private name is stored, which is fixed by its declaration and so
    // is a compile-time fact at every access: a field's value is per object, a
    // method's is one closure shared by every object that carries the brand,
    // and an accessor's is a pair of them. `private.get` therefore answers only
    // the storage question — the kind dispatch 6.2.12.2 writes as a run-time
    // step is resolved here instead.
    enum class PrivateKind { Field, Method, Accessor };
    struct PrivateElement {
        std::string name;  // `#x`, the `#` kept
        PrivateKind kind = PrivateKind::Field;
        bool isStatic = false;
        bool hasGetter = false;
        bool hasSetter = false;
    };
    // The private names of each class body being lowered, innermost last. A
    // mention resolves against it exactly as the parser resolved the reference:
    // innermost class first, so a nested class shadows an outer name it repeats
    // and reaches an outer name it does not.
    std::vector<std::vector<PrivateElement>> privateScopes_;
    const PrivateElement* findPrivateElement(const std::string& name) const;
    // The four environment slots a private name can own, named so that no
    // source identifier and no other private name can collide with one (`\x01`
    // is unspellable in an IdentifierName). The class-evaluation record holds
    // them, which is what makes every one of them per EVALUATION.
    static std::string privateTableSlot(const std::string& name);
    static std::string privateSetterTableSlot(const std::string& name);
    static std::string privateFnSlot(const std::string& name);
    static std::string privateSetterFnSlot(const std::string& name);
    // The value in one of those slots, read through the environment chain.
    // `nullopt` with a diagnostic when no enclosing record owns the slot, which
    // is a lowering bug: the parser has already refused every undeclared name.
    std::optional<Value> emitPrivateSlotRead(const std::string& slotName, Span span,
                                             il::Function& ilFn);
    // `o.#x`, with the receiver already lowered and boxed.
    std::optional<Value> lowerPrivateRead(const ast::MemberAccess& mem, Value objBoxed,
                                          il::Function& ilFn);
    // `o.#x = v` and the definition form the class body's initializers use.
    bool lowerPrivateWrite(const ast::MemberAccess& mem, Value objBoxed, Value valBoxed,
                           il::Function& ilFn);
    std::optional<Value> lowerPrivateAssignment(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerPrivateUpdate(const ast::MemberAccess& mem, ast::UnaryOp op,
                                            il::Function& ilFn);
    // `#x in o` (13.10.1), which answers a boolean and never throws.
    std::optional<Value> lowerPrivateIn(const ast::Expr& nameExpr, const ast::Expr& objExpr,
                                        il::Function& ilFn);
    // The always-throwing tail of a well-branded access that is still a
    // TypeError: writing a method, reading a set-only accessor, writing a
    // get-only one.
    Value emitPrivateMisuse(const std::string& name, int32_t code, il::Function& ilFn);
    Value emitPrivateCall(Value fnVal, Value thisVal, const std::vector<il::ValueId>& args,
                          il::Function& ilFn);
    // The private names a class body declares, in declaration order, with the
    // accessor halves of one name merged into one element.
    static std::vector<PrivateElement> collectPrivateElements(
        const std::vector<ast::ClassMethod>& methods);
    // The class-evaluation record: one slot per private table, one per shared
    // closure, and one for the class's OWN NAME — minted here so that two
    // evaluations of one class expression share nothing. Pushed onto the
    // environment stack, so a method body resolves `#x` by the same walk a
    // captured variable takes.
    //
    // The name slot is 15.7.14's classEnv binding (step 3), which is what lets
    // a static block say `C.#x`: the outer binding a `class C {}` declaration
    // introduces is still in its dead zone while the class is being evaluated.
    // It is lexical, so `class C extends C {}` is still the ReferenceError
    // 15.7.14 makes it. `className` may be empty (an anonymous class
    // expression), and then there is no slot.
    bool openClassScope(const std::string& className,
                        const std::vector<PrivateElement>& elements, il::Function& ilFn);
    // Fills that slot once the constructor exists — 15.7.14 step 28, after
    // every method is defined and before any static element runs.
    bool initClassNameBinding(const std::string& className, Value ctorVal, Span span,
                              il::Function& ilFn);
    // The statements a constructor runs before its own body: the brand adds for
    // every private method and accessor (6.2.12.4, which is what makes a later
    // access brand-check), then each field initializer in DEFINITION order.
    // Public fields are here too, because the order is one order.
    std::vector<ast::StmtPtr> buildFieldInitStatements(
        const std::vector<ast::ClassMethod>& methods,
        const std::vector<PrivateElement>& elements, Span span);
    // The static half of the same, emitted at class evaluation against the
    // constructor: static private methods and accessors get their brand there,
    // because the constructor IS the object that carries them.
    bool emitStaticPrivateBrands(const std::vector<PrivateElement>& elements, Value ctorVal,
                                 Span span, il::Function& ilFn);
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

// The BRONZE_ABI_FN_FLAG_* byte for a function the source wrote, from the three
// AST facts that decide it. One function because two lowering sites ask —
// a top-level `function` declaration and every nested closure — and a second
// copy of "a generator has a prototype and is not a constructor" is a second
// chance to disagree with 10.2.
uint32_t functionObjectFlags(ast::FunctionKind kind, bool isGenerator, bool isAsync);

}  // namespace bronze::lower
