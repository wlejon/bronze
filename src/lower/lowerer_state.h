#pragma once

// The plain data types the lowering units share: one per mechanism, each named
// for the thing it holds. They live here rather than nested in `Lowerer`
// because a struct with no behaviour is not part of the pass's interface, and
// because a header of members-only declarations is the one seam a single class
// can be split along.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"

namespace bronze::lower {

struct LowererValue {
    il::ValueId id = il::kNoValue;
    il::Type type = il::Type::Dynamic;
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
    bool inEnv = false;
    size_t envScopeIndex = 0;
    uint32_t envSlot = 0;
    size_t shadowedBinding = SIZE_MAX;
    bool isTdzHoisted = false;
};

enum class JumpKind { Loop, Switch, LabeledBlock };

struct JumpTarget {
    JumpKind kind = JumpKind::Loop;
    std::string label;
    il::BlockId headerBlock = il::kNoBlock;
    il::BlockId updateBlock = il::kNoBlock;
    il::BlockId exitBlock = il::kNoBlock;
    std::vector<std::string> vars;
    size_t cleanupDepthAtEntry = 0;
    size_t cleanupDepthInBody = 0;
    il::ValueId perIterationEnv = il::kNoValue;
};

enum class CleanupKind {
    Finally,
    IteratorClose,
    AsyncIteratorClose,
};

struct CleanupFrame {
    CleanupKind kind = CleanupKind::Finally;
    const ast::TryStmt* stmt = nullptr;
    il::ValueId iterRecord = il::kNoValue;
    uint32_t iterFrameSlot = UINT32_MAX;
    size_t jumpDepth = 0;
    il::BlockId outerHandler = il::kNoBlock;
};

// The two immutable-binding forms ECMA-262 creates, told apart by the S that
// 9.1.1.1.5 SetMutableBinding step 7 tests before it throws.
enum class SlotImmutability : uint8_t { Mutable, Silent, Throws };

struct EnvScopeInfo {
    std::unordered_map<std::string, uint32_t> slotOf;
    std::vector<std::string> slotNames;
    std::vector<bool> slotIsLexical;
    // Which of the lexical slots are DEFINITELY ASSIGNED: their initializer
    // runs before any user code can run in the scope, so no read of them can
    // reach the dead zone (ast/queries.h,
    // `getDefinitelyAssignedLexicalNames`). The binding is still lexical — it
    // is a `let`, `const` or `class` and every diagnostic that turns on that
    // is unchanged — but its slot carries no marker and its reads carry no
    // check.
    std::vector<bool> slotIsDefiniteInit;
    // What 9.1.1.1.5 step 7 answers for the slot's binding. `Silent` is
    // CreateImmutableBinding(n, false) — a named function expression's own name
    // — where the S the step tests is the ASSIGNING code's strictness;
    // `Throws` is CreateImmutableBinding(n, true), which `const` uses, where it
    // is the binding's own and the strictness of the assigning code never
    // enters into it.
    std::vector<SlotImmutability> slotImmutable;
    // Which slots hold a Number at every read (lower_scope.cpp
    // `planEnvSlotNumberTypes`). The record itself is unchanged — a canonical
    // double IS a Value by NaN-box construction and the collector already walks
    // past one — so what this marks is that the READ may stop testing.
    std::vector<bool> slotIsF64;
    // Which of those slots owe their `slotIsF64` to a `--pins` PROMISE rather
    // than to the fixpoint's proof — the ones `planEnvSlotNumberTypes` re-admits
    // in its pin arm. Only these get a write barrier: a slot the fixpoint
    // proved has had every write to it audited already, and checking it again
    // would tax a program that made no promise at all. The pin's manifest line
    // is `slotPinText`, held as TEXT rather than as a key index because the
    // planner that fills it is const and interning a key is not.
    std::vector<bool> slotIsPinned;
    std::vector<std::string> slotPinText;
    // The PIN CENSUS candidates (`--census`, src/runtime/pin_census.h): slots
    // with the binding STRUCTURE an env-slot pin needs that the fixpoint could
    // not type. Exactly the set the five `function WebGLState.<slot>: number`
    // lines of bench/pins/env-slot-kernel.pins were written by hand for — and
    // exactly the complement of `slotIsF64`, so a census can never propose a
    // slot the proof already owns (stage E4's HANDOFF (c)).
    //
    // `slotCensusText` is the entry the site would support, kind and all
    // decided at exit, held as text for the reason `slotPinText` is.
    std::vector<bool> slotIsCensus;
    std::vector<std::string> slotCensusText;
    // The STATIC CALL PLAN for this scope's slots (lower_scope.cpp
    // `planStableFunctionSlots`, spent by lower_call.cpp).
    //
    // `slotIsStableFn` marks a slot whose binding is a FUNCTION DECLARATION of
    // this scope that nothing in the scope's whole lexical reach can reassign —
    // decided from the AST before a single instruction is emitted, which is what
    // makes it a plan and not a guess. `slotFnIndex` is the IL function that
    // declaration lowered to, filled in by the hoisting pass that creates the
    // closure, and `kNoStableFn` until then (a call written above a declaration
    // in a body lowered before it simply does not get the edge).
    //
    // Together they license a call to that name to be a DIRECT call: the target
    // is known, and the environment it captured is this scope's own record,
    // which every caller can reach by counting parent links.
    std::vector<bool> slotIsStableFn;
    std::vector<uint32_t> slotFnIndex;
    il::ValueId envValue = il::kNoValue;
    uint32_t childSlot = UINT32_MAX;
};

// "No function is known for this slot" — see EnvScopeInfo::slotFnIndex.
inline constexpr uint32_t kNoStableFn = 0xFFFFFFFFu;

struct VarState {
    il::ValueId valueId = il::kNoValue;
    il::Type type = il::Type::Dynamic;
};
using VarStateMap = std::unordered_map<std::string, VarState>;

struct ExprJoin {
    std::vector<std::string> vars;
    std::unordered_map<std::string, il::ValueId> paramId;
    std::unordered_map<std::string, il::Type> paramType;
};

struct StaticSlotSite {
    uint32_t slot = 0;
    uint32_t cellIndex = 0;
    uint32_t familyLo = il::Instruction::kNoFamily;
    uint32_t familySpan = 0;
};

struct GeneratorContext {
    size_t frameScope = SIZE_MAX;
    il::ValueId frameEnv = il::kNoValue;
    il::ValueId modeParam = il::kNoValue;
    il::ValueId sentParam = il::kNoValue;
    uint32_t stateSlot = 0;
    uint32_t iterSlot = UINT32_MAX;
    bool isAsync = false;
    bool isAsyncGenerator = false;
    uint32_t machineSlot = UINT32_MAX;
    std::vector<uint32_t> loopIterSlots;
    uint32_t activeIterLoops = 0;

    static constexpr double kModeNext = 0.0;
    static constexpr double kModeReturn = 1.0;
    static constexpr double kModeThrow = 2.0;
    std::vector<il::BlockId> resumeBlocks;
};

// What `planClosureParamNumbers` proved for ONE function body, keyed by the
// declaration node. A nested declaration appears in exactly one enclosing
// statement list, so a frame holds one entry per function.
using ProvenParamPlan = std::unordered_map<const ast::FunctionDecl*, std::vector<bool>>;

// One loop variable and the type every block parameter standing for it takes —
// header, exit, and the update/condition join alike, because the analysis
// proves one type covering all of them.
struct LoopParam {
    std::string name;
    il::Type type = il::Type::Dynamic;
};

// How a pattern's names reach their bindings. A declaration MAKES them and an
// assignment writes ones that already exist, which is the only difference
// between the two forms once the pattern itself is walked.
struct PatternTarget {
    bool declare = true;
    bool isConst = false;
    bool isLet = true;
    bool isVar = false;
};

// A property reference used as a destructuring target, held open across the
// element read. 13.15.5.2 evaluates the reference BEFORE the source element
// it will receive, so `[o[i()]] = xs` calls `i` before the iterator steps —
// which means the base and the key have to be lowered at one point and the
// store emitted at another.
struct PatternRef {
    LowererValue object{il::kNoValue, il::Type::Dynamic};
    // Exactly one of these: a constant key index, or a computed key value.
    uint32_t keyIndex = 0;
    bool hasKeyIndex = false;
    LowererValue index{il::kNoValue, il::Type::Dynamic};
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

// One short-circuit edge out of an optional chain: where it leaves from, and
// what every binding held there. The chain's join takes a parameter for the
// result and one per binding the edges disagree about, so the edges have to be
// COLLECTED before the join's parameters can be sized — which is why the jumps
// are emitted at the end rather than as each link is lowered.
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

// What `this` and the parameters mean while a certified module-literal method's
// body is being emitted at its call site. `receiver` is the value the SITE read
// for the binding, so the dead-zone check that read carries is the one the body
// runs under.
struct InlineFrame {
    const std::string* binding = nullptr;
    LowererValue receiver;
    std::map<std::string, LowererValue> params;
};

}  // namespace bronze::lower
