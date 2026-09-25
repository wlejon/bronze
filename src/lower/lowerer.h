#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "ast/clone.h"
#include "il/il.h"
#include "lower/bigint_reach.h"
#include "lower/direct_method_table.h"
#include "lower/infer_stats.h"
#include "lower/lowerer_state.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/pins.h"
#include "types/result.h"

namespace bronze::lower {

class NativeManifest;
struct NativeSig;

// The AST -> IL pass. One instance per module; its methods are defined
// across the lower_*.cpp units named in the seam comments below.
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
    using ProvenParamPlan = lower::ProvenParamPlan;
    using LoopParam = lower::LoopParam;
    using PatternTarget = lower::PatternTarget;
    using PatternRef = lower::PatternRef;
    using PrivateKind = lower::PrivateKind;
    using PrivateElement = lower::PrivateElement;
    using ChainExit = lower::ChainExit;
    using ChainMiss = lower::ChainMiss;
    using InlineFrame = lower::InlineFrame;
    using CachedTypedElemGet = lower::CachedTypedElemGet;

    Lowerer(const ast::Module& astModule, DiagnosticSink& diags,
            const types::InferenceResult* inference,
            const std::vector<std::string>* hostGlobals = nullptr,
            const SourceSet* sources = nullptr,
            InferStatsCollector* stats = nullptr,
            bool assumeNoBigInt = false,
            const types::PinManifest* pins = nullptr,
            const std::string& censusOutPath = {},
            const NativeManifest* nativeManifest = nullptr);

    std::optional<il::Module> lower();

private:
    const ast::Module& astModule_;
    DiagnosticSink& diags_;
    const types::InferenceResult* inference_ = nullptr;
    const types::PinManifest* pins_ = nullptr;
    il::Module ilModule_;
    std::unordered_set<std::string> hostGlobals_;
    std::unordered_map<std::string, uint32_t> functionIndices_;
    std::unordered_map<uint32_t, Value> functionRefMap_;
    std::unordered_map<std::string, uint32_t> keyConstants_;
    std::vector<std::string> keyStrings_;
    uint32_t icSiteCounter_ = 0;
    uint32_t anonFnCounter_ = 0;

    // --- Direct method-call edge (direct_method_table.h) ---
    DirectMethodTable directMethods_;
    uint32_t lastClosureFnIndex_ = il::Instruction::kNoDirectTarget;
    void recordMethodCallSite(const il::Instruction& inst, const ast::Expr& receiver,
                              const std::string& method);

    uint32_t staticSiteCounter_ = 0;
    uint32_t templateSiteCounter_ = 0;

    std::vector<VarBinding> varBindings_;
    std::unordered_map<std::string, size_t> activeVarMap_;
    size_t currentScopeDepth_ = 0;
    uint32_t varDeclCounter_ = 0;
    std::vector<JumpTarget> jumpStack_;
    std::vector<std::string> labelStack_;
    il::BlockId currentHandler_ = il::kNoBlock;
    std::vector<CleanupFrame> cleanupStack_;
    std::string pendingLabel_;
    size_t currentBlockIdx_ = 0;
    Span currentStmtSpan_;

    std::vector<EnvScopeInfo> envScopes_;
    std::vector<il::ValueId> savedEnvValues_;
    std::vector<bool> scopeHasEnv_;
    il::ValueId currentEnvValue_ = il::kNoValue;
    il::ValueId entryEnvValue_ = il::kNoValue;
    std::unordered_map<uint64_t, Value> immutableEnvCache_;
    std::unordered_set<std::string> assignedNames_;
    std::optional<CachedTypedElemGet> cachedTypedElemGet_;
    bool inUserFunction_ = false;
    il::ValueId currentEnv(il::Function& ilFn);
    il::ValueId currentThisValue_ = il::kNoValue;
    bool currentFunctionIsArrow_ = false;
    bool derivedCtorThis_ = false;
    bool pendingDerivedCtor_ = false;
    bool strictCode_ = false;
    int32_t strictFlag() const { return strictCode_ ? 1 : 0; }
    std::unordered_set<std::string> capturedNames_;
    std::unordered_set<std::string> memoryNames_;
    mutable std::unordered_set<std::string> assumedNumericIdents_;
    size_t functionEnvBase_ = 0;
    size_t functionEnvScope_ = SIZE_MAX;
    std::vector<std::string> moduleEnvSlots_;
    size_t moduleEnvScope_ = SIZE_MAX;
    bool segmentTopLevel_ = false;

    // --- lower_infer.cpp: what inference proved ---
    static il::Type ilTypeOf(types::Type t);
    const ast::Node* inferenceNode(const ast::Node& n) const;
    const ast::Expr& inferenceExpr(const ast::Expr& e) const;
    const ast::Stmt& inferenceStmt(const ast::Stmt& s) const;
    std::vector<const ast::CloneOrigins*> cloneOrigins_;

    types::Type inferredType(const ast::Expr& expr) const;
    bool provenNumber(const ast::Expr& expr) const;
    bool monomorphicPropSite(const ast::Expr& receiver) const;
    bool functionBindingReceiver(const ast::Expr& receiver, uint32_t keyIndex) const;
    bool pristineMathCall(const ast::Expr& call) const;

    // --- lower_typed_elem.cpp: proven typed-array element access ---
    static bool typedElemSeamDisabled();
    std::optional<uint32_t> typedElemAccessKind(const ast::Expr& e, bool coercing = false) const;
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
    const std::vector<ast::StmtPtr>* currentBodyStmts_ = nullptr;
    bool typedElemDisabled_ = false;
    bool staticShapesDisabled_ = false;
    static bool staticShapeSeamDisabled();
    bool familyGuardDisabled_ = false;
    static bool familyGuardSeamDisabled();
    bool unboxedFieldsDisabled_ = false;
    static bool unboxedFieldSeamDisabled();
    std::vector<ProvenParamPlan> provenClosureParams_;
    bool numericArithDisabled_ = false;
    static bool numericArithSeamDisabled();
    bool provenFieldRead(const ast::Expr& e) const;
    Value emitRawUnbox(Value boxed, il::Function& ilFn);
    bool nullishNumberFieldRead(const ast::Expr& e) const;
    Value emitNullishUnbox(Value boxed, il::Function& ilFn);
    void buildClassFamilyTable();
    bool classFamilyTableBuilt_ = false;
    void buildSlotReprTable();
    std::optional<StaticSlotSite> claimStaticSlot(const ast::Expr& receiver,
                                                  const std::string& key, bool forWrite);
    void stampStaticSlot(il::Instruction& inst, const ast::Expr& receiver);
    void reportClassLayouts();
    il::Type mergeParamType(const ast::Stmt& mergePoint, const std::string& name) const;
    const types::Signature* provenSignature(uint32_t moduleFnIndex) const;
    types::Type provenParamType(uint32_t moduleFnIndex, size_t paramIndex) const;
    types::Type provenReturnType(uint32_t moduleFnIndex) const;
    types::Type provenClosureReturn(const ast::Node& site) const;
    bool applyProvenSignature(const ast::FunctionDecl& fnDecl, uint32_t moduleFnIndex,
                              il::Function& fn);
    bool applySignaturePins(const std::vector<ast::Param>& params, Span span, il::Function& fn);
    bool checkAnnotation(const std::string& ann, Span span, const std::string& name,
                         types::Type proven);
    std::string propBailReason(const ast::Expr& expr) const;
    void recordPropertySite(uint16_t fileId, PropSiteVerdict verdict,
                            const std::string& bailReason = "");
    void recordCall(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    void recordElementOp(uint16_t fileId, bool isNative, const std::string& bailReason = "");

    const SourceSet* sources_ = nullptr;
    InferStatsCollector* stats_ = nullptr;

    // --- lower_generator.cpp: generator state machine ---
    std::optional<GeneratorContext> generator_;
    static const char* generatorStateSlotName();
    static const char* generatorEnvSlotName();
    static const char* generatorIterSlotName();
    static const char* asyncMachineSlotName();
    static std::string loopIterSlotName(uint32_t depth);
    static std::string finallyPendingSlotName(uint32_t depth);
    static const char* generatorReturnSlotName();
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
    std::optional<Value> lowerYieldStar(const ast::YieldExpr& yield, il::Function& ilFn);
    bool lowerResumeBody(const std::vector<const ast::Stmt*>& stmts, il::Function& resumeFn,
                         bool isAsync = false, bool isAsyncGenerator = false);
    bool lowerGeneratorTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);
    bool lowerAsyncGeneratorTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);

    // --- lower_async.cpp: async driver ---
    std::optional<Value> lowerAwait(const ast::YieldExpr& await, il::Function& ilFn);
    std::optional<Value> lowerAwaitValue(Value awaited, Span span, il::Function& ilFn);
    bool lowerAsyncTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);

    // --- lower.cpp: module skeleton and function bodies ---
    void enterFunctionEnv(const std::vector<ast::Param>& params,
                          const std::vector<const ast::Stmt*>& body, il::Function& ilFn,
                          bool isGenerator = false, bool isAsync = false);
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
    bool lowerBodyWithPlan(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                           bool isGenerator, bool isAsync);

    // --- lower_unresolved.cpp: unresolved name references ---
    bool resolvesName(const std::string& name) const;
    void warnUnresolved(const std::string& name, Span span);
    Value emitReferenceError(const std::string& name, Span span, il::Function& ilFn);
    std::vector<std::string> functionVarNames_;
    std::unordered_set<std::string> warnedUnresolved_;

    // --- lower_util.cpp: key constants, blocks, coercions, truthiness ---
    bool isProvidedGlobal(const std::string& name) const;
    uint32_t getKeyConstantIndex(const std::string& key);
    std::string moduleUrl(uint16_t fileId) const;
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

    // --- lower_native.cpp: direct calls to host natives (native_manifest.h) ---
    const NativeManifest* nativeManifest_ = nullptr;
    std::unordered_map<size_t, std::string> varNativeClasses_;
    std::unordered_map<std::string, uint32_t> nativeImportIndex_;
    void initNativeManifestGlobals();
    std::string getDottedPath(const ast::Expr* expr) const;
    bool isFreeIdentifier(const std::string& name) const;
    std::string getNativeClassOfExpr(const ast::Expr* expr) const;
    void noteNativeClassOfBinding(const std::string& name, const std::string& cls);
    std::string capturedConstNativeClass(const std::string& name) const;
    void planEnvSlotNativeClasses(size_t scopeIndex, const std::vector<const ast::Stmt*>& stmts);
    void planEnvSlotNativeClasses(size_t scopeIndex, const std::vector<ast::StmtPtr>& stmts);
    std::optional<Value> tryLowerNativeCall(const ast::Call* call, il::Function& ilFn);
    std::optional<Value> tryLowerNativeNew(const ast::NewExpr* newExpr, il::Function& ilFn);
    std::optional<Value> tryLowerNativePropertyGet(const ast::MemberAccess* mem, il::Function& ilFn, bool onSpine);
    std::optional<Value> tryLowerNativeAssignment(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> emitNativeInvoke(const NativeSig& sig, const ast::Expr* selfExpr,
                                          std::optional<Value> selfValue,
                                          const std::vector<const ast::Expr*>& args,
                                          std::optional<Value> preLowered, il::Function& ilFn);
    Value emitDefaultValueForType(il::Type type, il::Function& ilFn);
    Value emitNativeClassTag(const std::string& className, il::Function& ilFn);
    uint32_t nativeImportFunction(const NativeSig& sig);
    uint32_t nativeClassTagFunction(const std::string& className);
    uint32_t registerExternalFunction(const std::string& symbol, il::Type returnType,
                                      const std::vector<il::Type>& paramTypes);
    void emitNativeBindPrologue();

    // --- lower_scope.cpp: scopes, environments, closures ---
    bool declareVariable(const std::string& name, il::Type type, bool isConst, bool isLet,
                         bool isVar, bool isInitialized, il::ValueId valId, Span span);
    il::ValueId emitConstUndefined(il::Function& ilFn);
    il::ValueId emitEnvCreate(uint32_t slotCount, il::Function& ilFn);
    il::ValueId emitModuleEnvGet(il::Function& ilFn);
    void emitModuleEnvSet(il::ValueId env, il::Function& ilFn);
    Value emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn);
    void emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn,
                    bool assigning = false);
    bool envSlotIsLexical(uint32_t depth, uint32_t index) const;
    bool envSlotDefiniteInit(uint32_t depth, uint32_t index) const;
    static bool definiteInitDisabled();
    SlotImmutability envSlotImmutability(uint32_t depth, uint32_t index) const;
    bool envSlotIsF64(uint32_t depth, uint32_t index) const;
    void planEnvSlotNumberTypes(const std::vector<ast::Param>& params,
                                const std::vector<const ast::Stmt*>& body,
                                const std::string& functionName, EnvScopeInfo& info) const;
    void emitImmutableAssign(const std::string& name, il::Function& ilFn);

    // --- --pins write barriers (lower_pin.cpp) ---
    bool emitPinGuard(Value val, const std::string& pinText, il::PinBarrier kind,
                      il::Function& ilFn);
    static bool pinSatisfiedStatically(Value val, il::PinBarrier kind);
    const types::PinKind* pinnedFieldAt(const ast::Expr& receiver, const std::string& key,
                                        std::string* pinTextOut) const;
    void emitPinFieldBarrier(const ast::Expr& receiver, const std::string& key, Value val,
                             il::Function& ilFn);
    bool emitPinnedElementBarrier(uint32_t elemKind, Value val, il::Function& ilFn,
                                  bool pinned = true);

    // --- Pin census (lower_census.cpp) ---
    bool censusEnabled() const { return !censusOutPath_.empty(); }
    static std::string manifestOwnerName(const std::string& ilName);
    std::string censusOutPath_;
    void emitCensusRecord(Value val, const std::string& target, il::CensusSite kind,
                          il::Function& ilFn);
    void addCensusSite(const std::string& target, il::CensusSite kind, bool refuses);
    void emitCensusFieldRecord(const ast::Expr& receiver, const std::string& key, Value val,
                               il::Function& ilFn);
    std::string censusFieldOwner(const ast::Expr& receiver, bool* opaque) const;
    void refuseAmbiguousCensusOwners();
    std::vector<std::pair<std::string, il::CensusSite>> censusSignatureOwners_;
    void openLexicalBindings(size_t scopeIndex, const std::vector<std::string>& lexicalNames,
                             const std::vector<std::string>& definiteNames,
                             const std::vector<std::string>& constNames,
                             il::Function& ilFn);
    uint32_t envDepthOf(size_t scopeIndex) const;
    Value readBinding(const VarBinding& b, il::Function& ilFn);
    void writeBinding(VarBinding& b, Value val, il::Function& ilFn);
    bool refuseConstAssignment(const VarBinding& b, il::Function& ilFn);
    bool findEnclosingEnvVar(const std::string& name, uint32_t& depth, uint32_t& index) const;

    // --- Static call plan & closure parameters (lower_static_call_plan.cpp, lower_closure_param_proof.cpp) ---
    void planStableFunctionSlots(const std::vector<const ast::Stmt*>& stmts,
                                 const std::vector<ast::Param>* params, EnvScopeInfo& info) const;
    void planStableFunctionSlots(const std::vector<ast::StmtPtr>& stmts,
                                 const std::vector<ast::Param>* params, EnvScopeInfo& info) const;
    void recordStableFunctionSlot(size_t scopeIndex, uint32_t slot, uint32_t fnIndex);
    static void planScopeRebinds(const std::vector<const ast::Stmt*>& stmts,
                                 const std::vector<ast::Param>* params, EnvScopeInfo& info);
    static void planScopeRebinds(const std::vector<ast::StmtPtr>& stmts,
                                 const std::vector<ast::Param>* params, EnvScopeInfo& info);
    void planClosureParamNumbers(const std::vector<ast::Param>& params,
                                 const std::vector<ast::StmtPtr>& stmts);
    void applyProvenClosureParams(const ast::Node& site, const std::vector<ast::Param>& params,
                                  il::Function& fn) const;
    static bool closureParamProofDisabled();
    bool findStableFunctionCallee(const std::string& name, uint32_t& envHops,
                                  uint32_t& fnIndex) const;
    std::optional<Value> lowerDirectCall(const ast::Call* call, uint32_t calleeIdx,
                                         il::ValueId envBase, uint32_t envHops,
                                         il::Function& ilFn);

    // --- Scopes & closures (lower_scope.cpp, lower_closure.cpp) ---
    void enterScope();
    void pushSyntheticEnv(std::vector<std::string> slots, il::Function& ilFn);
    void enterScope(const std::vector<ast::StmtPtr>& stmts, il::Function& ilFn,
                    const std::vector<std::string>& extraDeclarations = {},
                    const std::vector<std::string>& extraLexicalDeclarations = {});
    void exitScope();
    std::optional<Value> lowerClosure(const ast::Node& site, const std::string& declaredName,
                                      const std::optional<std::string>& jsName,
                                      const std::vector<ast::Param>& params,
                                      const std::string& returnTypeAnn,
                                      const std::vector<ast::StmtPtr>& body, Span span,
                                      il::Function& ilFn, bool isArrow = false,
                                      bool bindsOwnName = false);
    std::optional<Value> lowerNamedEvaluation(const ast::Expr& expr, const std::string& name,
                                              il::Function& ilFn);
    // The name the SOURCE spelled for a binding: the module linker renames an
    // imported file's module-scope bindings to `mod<N>.<name>`
    // (modules::canonicalName), and a function's `name`, a class's and every
    // Error.stack frame must still read `<name>`. Only a BINDING name goes
    // through this, never a property key, which may contain a `.` of its own.
    static std::string sourceSpelling(const std::string& bindingName);

    // --- lower_pattern.cpp: binding patterns, defaults, spread ---
    bool lowerPattern(const ast::BindingPattern& pattern, Value source,
                      const PatternTarget& target, il::Function& ilFn);
    bool lowerArrayPattern(const ast::BindingPattern& pattern, Value source,
                           const PatternTarget& target, il::Function& ilFn);
    bool lowerObjectPattern(const ast::BindingPattern& pattern, Value source,
                            const PatternTarget& target, il::Function& ilFn);
    bool bindPatternName(const std::string& name, Value value, const PatternTarget& target,
                         Span span, il::Function& ilFn);
    std::optional<PatternRef> evalPatternRef(const ast::Expr& target, il::Function& ilFn);
    bool storePatternRef(const PatternRef& ref, Value value, il::Function& ilFn);
    std::optional<Value> emitDefaultIfUndefined(Value current, const ast::Expr& defaultExpr,
                                                const std::string& bindingName,
                                                il::Function& ilFn);
    Value emitPatternCheck(Value source, bool isObject, il::Function& ilFn);
    std::optional<Value> lowerDestructuringAssign(const ast::DestructuringAssign* node,
                                                  il::Function& ilFn);
    bool lowerParamBindings(const std::vector<ast::Param>& params, uint32_t paramBase,
                            il::Function& ilFn);
    static void applyParamShape(const std::vector<ast::Param>& params, il::Function& fn);
    static bool listHasSpread(const std::vector<ast::ExprPtr>& list);
    static constexpr size_t kFixedArgOperandLimit = 16;
    static bool argsTakeArrayPath(const std::vector<ast::ExprPtr>& args);
    Value emitValuesAsArray(const std::vector<il::ValueId>& values, il::Function& ilFn);
    std::optional<Value> lowerListToArray(const std::vector<ast::ExprPtr>& list,
                                          il::Function& ilFn);
    void emitContainerOp(il::Op op, Value container, Value value, il::Function& ilFn);

    // --- lower_stmt.cpp: statements ---
    bool lowerStmtList(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn);
    bool lowerStmt(const ast::Stmt& stmt, il::Function& ilFn);
    bool lowerVarDecl(const ast::VarDecl* varDecl, il::Function& ilFn);

    // --- lower_class.cpp: classes ---
    bool lowerClassDecl(const ast::ClassDecl* cls, il::Function& ilFn);
    // `inferredName`: the name NamedEvaluation gives an anonymous class
    // expression (`var K = class {}` makes `K.name === "K"`); it names the
    // class and binds nothing.
    std::optional<Value> lowerClassExpr(const ast::ClassExpr* cls, il::Function& ilFn,
                                        const std::string& inferredName = {});
    std::optional<Value> lowerClass(const std::string& name, const ast::Expr* superClass,
                                    const std::string& superName,
                                    const std::vector<ast::ClassMethod>& methods, Span span,
                                    il::Function& ilFn, bool bindsOwnName = false,
                                    const std::string& inferredName = {});
    Value emitPrototypeOf(Value ctorVal, il::Function& ilFn);
    std::optional<Value> lowerSuperLookupStart(const ast::SuperMember& sm, il::Function& ilFn);
    void emitDerivedCtorReturn(Value val, il::Function& ilFn);

    // --- lower_private.cpp: private class elements ---
    std::vector<std::vector<PrivateElement>> privateScopes_;
    const PrivateElement* findPrivateElement(const std::string& name) const;
    static std::string privateTableSlot(const std::string& name);
    static std::string privateSetterTableSlot(const std::string& name);
    static std::string privateFnSlot(const std::string& name);
    static std::string privateSetterFnSlot(const std::string& name);
    std::optional<Value> emitPrivateSlotRead(const std::string& slotName, Span span,
                                             il::Function& ilFn);
    std::optional<Value> lowerPrivateRead(const ast::MemberAccess& mem, Value objBoxed,
                                          il::Function& ilFn);
    bool lowerPrivateWrite(const ast::MemberAccess& mem, Value objBoxed, Value valBoxed,
                           il::Function& ilFn);
    std::optional<Value> lowerPrivateAssignment(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerPrivateUpdate(const ast::MemberAccess& mem, ast::UnaryOp op,
                                            il::Function& ilFn);
    std::optional<Value> lowerPrivateIn(const ast::Expr& nameExpr, const ast::Expr& objExpr,
                                        il::Function& ilFn);
    Value emitPrivateMisuse(const std::string& name, int32_t code, il::Function& ilFn);
    Value emitPrivateCall(Value fnVal, Value thisVal, const std::vector<il::ValueId>& args,
                          il::Function& ilFn);
    static std::vector<PrivateElement> collectPrivateElements(
        const std::vector<ast::ClassMethod>& methods);
    bool openClassScope(const std::string& className,
                        const std::vector<PrivateElement>& elements, il::Function& ilFn);
    bool initClassNameBinding(const std::string& className, Value ctorVal, Span span,
                              il::Function& ilFn);
    std::vector<ast::StmtPtr> buildFieldInitStatements(
        const std::vector<ast::ClassMethod>& methods,
        const std::vector<PrivateElement>& elements, Span span);
    bool emitStaticPrivateBrands(const std::vector<PrivateElement>& elements, Value ctorVal,
                                 Span span, il::Function& ilFn);
    std::optional<Value> lowerSuperMember(const ast::SuperMember* sm, il::Function& ilFn);
    std::optional<Value> lowerSuperCall(const ast::SuperCall* sc, il::Function& ilFn);
    std::optional<Value> lowerThisValue(Span span, il::Function& ilFn);
    bool lowerReturnStmt(const ast::ReturnStmt* retStmt, il::Function& ilFn);

    // --- lower_control.cpp: control flow, block-argument SSA ---
    std::vector<il::ValueId> collectEdgeArgs(const std::vector<std::string>& vars,
                                             il::BlockId target, il::Function& ilFn,
                                             size_t scopeDepth = SIZE_MAX);
    std::vector<std::string> getActiveVarsInDeclOrder() const;
    std::vector<LoopParam> collectLoopParams(const ast::Stmt& loopStmt,
                                             const std::unordered_set<std::string>& assigned);
    std::unordered_map<std::string, il::ValueId> addLoopBlockParams(
        const std::vector<LoopParam>& loopParams, il::BlockId block, il::Function& ilFn);
    void bindLoopBlockParams(const std::vector<LoopParam>& loopParams,
                             const std::unordered_map<std::string, il::ValueId>& paramOf);
    il::ValueId addEnvBlockParam(il::BlockId block, il::Function& ilFn);
    bool forNeedsPerIterationEnv(const ast::ForStmt& forStmt) const;
    il::ValueId emitPerIterationEnv(il::ValueId source, uint32_t slotCount, il::ValueId parent,
                                    il::Function& ilFn);
    bool lowerIfStmt(const ast::IfStmt* ifStmt, il::Function& ilFn);
    bool lowerWhileStmt(const ast::WhileStmt* whileStmt, il::Function& ilFn);
    bool lowerDoWhileStmt(const ast::DoWhileStmt* doWhileStmt, il::Function& ilFn);
    bool lowerForStmt(const ast::ForStmt* forStmt, il::Function& ilFn);
    bool lowerBreakStmt(const ast::BreakStmt* breakStmt, il::Function& ilFn);
    bool emitJumpCrossingFinallys(size_t targetIndex, bool toExit, il::Function& ilFn);
    bool lowerContinueStmt(const ast::ContinueStmt* continueStmt, il::Function& ilFn);
    std::string takePendingLabel();

    // --- lower_label.cpp: labelled statements and jump targets ---
    bool lowerLabeledStmt(const ast::LabeledStmt* labeled, il::Function& ilFn);
    bool lowerLabeledBlock(const ast::LabeledStmt* labeled, il::Function& ilFn);
    const JumpTarget* findJumpTarget(const std::string& label, bool forContinue, Span span);
    void emitJumpToTarget(const JumpTarget& target, il::BlockId block,
                          const std::vector<il::ValueId>& extraArgs, il::Function& ilFn);

    // --- lower_iter_loop.cpp: container loops (for-of, for-in) ---
    bool lowerForOfStmt(const ast::ForOfStmt* forOf, il::Function& ilFn);
    bool lowerForInStmt(const ast::ForInStmt* forIn, il::Function& ilFn);
    bool lowerIteratorLoop(const ast::Stmt& loopStmt, Value iterVal, const std::string& headName,
                           const ast::BindingPattern* headPattern, bool isConst, bool isLet,
                           bool isVar, const std::vector<ast::StmtPtr>& body,
                           il::Function& ilFn, bool isAwait = false);

    // --- lower_switch.cpp: selection and fallthrough ---
    bool lowerSwitchStmt(const ast::SwitchStmt* sw, il::Function& ilFn);

    // --- lower_try.cpp: try/catch/finally and throw ---
    bool lowerTryStmt(const ast::TryStmt* tryStmt, il::Function& ilFn);
    bool lowerTryCatch(const ast::TryStmt* tryStmt, il::Function& ilFn);
    bool lowerTryBlock(const ast::TryStmt* tryStmt, il::Function& ilFn);
    bool lowerThrowStmt(const ast::ThrowStmt* throwStmt, il::Function& ilFn);
    bool runCleanups(size_t downTo, il::Function& ilFn);
    size_t cleanupDepthForJump(size_t targetIndex) const;
    void emitIterClose(il::ValueId record, bool suppress, il::Function& ilFn, bool isAsync = false);
    bool lowerFinallyBody(const ast::TryStmt& stmt, il::Function& ilFn);
    void openBlockUnderHandler(il::BlockId handler, il::Function& ilFn);

    // --- lower_expr_cond.cpp: conditional expressions ---
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

    // --- lower_expr_chain.cpp: optional chains ---
    std::optional<Value> lowerOptionalChain(const ast::Expr& expr, il::Function& ilFn);
    std::optional<Value> lowerChainJoin(const std::function<std::optional<Value>()>& body,
                                        ChainMiss miss, il::Function& ilFn);
    std::optional<Value> lowerChainBase(const ast::Expr& base, il::Function& ilFn, bool onSpine);
    void emitChainShortCircuit(Value base, il::Function& ilFn);
    std::vector<ChainExit> chainExits_;
    bool spinePos_ = false;

    // --- lower_expr.cpp: expressions and assignments ---
    std::optional<Value> lowerExpr(const ast::Expr& expr, il::Function& ilFn);
    std::optional<Value> lowerAssignment(const ast::Binary* bin, il::Function& ilFn);

    // --- lower_update.cpp: updates (++ / --) ---
    std::optional<Value> lowerUpdate(const ast::Unary& un, il::Function& ilFn);
    std::optional<Value> lowerMemberUpdate(const ast::MemberAccess& mem, ast::UnaryOp op,
                                           il::Function& ilFn);
    std::optional<Value> lowerIndexUpdate(const ast::IndexAccess& idx, ast::UnaryOp op,
                                          il::Function& ilFn);
    Value emitUpdateOld(Value oldVal, il::Function& ilFn);
    Value emitUpdateStep(Value oldNumeric, ast::UnaryOp op, il::Function& ilFn);

    // --- lower_define_props.cpp: Object.defineProperties literal ---
    std::optional<Value> lowerDefinePropertiesLiteral(const ast::Call* call,
                                                      const ast::MemberAccess& callee,
                                                      il::Function& ilFn);

    // --- lower_expr_binary.cpp: binary operators ---
    std::optional<Value> lowerBinary(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerAddChain(const ast::Binary* bin, il::Function& ilFn);
    std::optional<Value> lowerEquality(ast::BinaryOp op, Value lhs, Value rhs,
                                       il::Function& ilFn);
    std::optional<Value> lowerRelational(ast::BinaryOp op, Value lhs, Value rhs,
                                         il::Function& ilFn);
    Value emitToInt32(Value val, il::Function& ilFn);
    Value emitBitwise(il::Op op, Value lhs, Value rhs, il::Function& ilFn);
    Value emitPow(Value lhs, Value rhs, il::Function& ilFn);
    Value emitLogicalNot(Value boolVal, il::Function& ilFn);
    static std::optional<il::Op> bitwiseOpFor(ast::BinaryOp op);

    // --- lower_object.cpp: objects, property access, new, calls ---
    std::optional<Value> lowerObjectLit(const ast::ObjectLit* objLit, il::Function& ilFn);
    std::optional<Value> lowerDelete(const ast::Unary& del, il::Function& ilFn);
    std::optional<Value> lowerDeleteReference(const ast::Unary& del, il::Function& ilFn);
    bool emitAccessorDef(Value target, const std::string& key, ast::AccessorKind kind,
                         const ast::FunctionExpr& fn, bool enumerable, il::Function& ilFn);
    bool emitAccessorDefComputed(Value target, Value key, ast::AccessorKind kind,
                                 const ast::FunctionExpr& fn, bool enumerable, il::Function& ilFn,
                                 const std::optional<std::string>& jsName = std::nullopt);
    std::optional<Value> lowerArrayLit(const ast::ArrayLit* arrLit, il::Function& ilFn);
    std::optional<Value> lowerNewExpr(const ast::NewExpr* newExpr, il::Function& ilFn);
    std::optional<Value> lowerMemberAccess(const ast::MemberAccess* mem, il::Function& ilFn,
                                           bool onSpine = false);
    std::optional<Value> lowerIndexAccess(const ast::IndexAccess* idxAccess, il::Function& ilFn,
                                          bool onSpine = false);
    std::optional<Value> emitIndexRead(const ast::IndexAccess& idxAccess, Value objBoxed,
                                       il::Function& ilFn);
    std::optional<Value> lowerCall(const ast::Call* call, il::Function& ilFn,
                                   bool onSpine = false);

    // --- lower_module_inline.cpp: module literal method inlining ---
    bool tryLowerModuleLiteralInline(const ast::Call& call, const ast::MemberAccess& mem,
                                     bool onSpine, il::Function& ilFn, std::optional<Value>& out);
    std::optional<Value> emitInlinedMethod(const std::string& binding, const std::string& key,
                                           const types::ModuleLiteralInline& method,
                                           Value receiver, const std::vector<Value>& args,
                                           il::Function& ilFn);
    std::optional<Value> emitInlineGuard(const types::ModuleLiteralInline& method, size_t guard,
                                         const std::string& key, const InlineFrame& frame,
                                         const std::vector<Value>& args, il::Function& ilFn);
    std::optional<Value> emitInlineExpr(const ast::Expr& expr, const InlineFrame& frame,
                                        il::Function& ilFn);
    Value emitInlinePropGet(Value receiver, const std::string& key, il::Function& ilFn);
    Value emitInlineMethodCall(Value receiver, const std::string& key,
                               const std::vector<Value>& args, il::Function& ilFn);
    std::optional<Value> emitInlineSelect(Value cond,
                                          const std::function<std::optional<Value>()>& thenArm,
                                          const std::function<std::optional<Value>()>& elseArm,
                                          il::Function& ilFn);
    std::vector<std::pair<std::string, std::string>> inlineStack_;
    std::map<std::string, uint32_t> inlinedLiteralSites_;
};

}  // namespace bronze::lower
