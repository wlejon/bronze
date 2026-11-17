#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "abi/bronze_abi.h"
// The pin claims the IL carries (`PinBarrier`, element flags).
// Included here rather than left to each user, because every one of them
// reaches these names through this header and a seam is not an occasion to
// make twenty files say where they now live.
#include "il/il_pin.h"
#include "il/il_op.h"
#include "support/source.h"

namespace bronze::il {

// bronze IL: a typed, SSA, mid-level IR. Static layouts are used wherever
// analysis can PROVE them (inference-first; TS annotations are untrusted
// hints); Dynamic is the explicit fallback type for code the analysis
// cannot type — wild JS must always compile. Dynamic is the fallback and
// never the substrate: a value is boxed because nothing PROVED it, not
// because boxing is the default the proofs opt out of.
//
// Deliberately tiny today: enough structure to carry lower→codegen work and
// to pin the canonical text form. Every addition must keep print(parse(x))
// byte-stable once the text parser lands.

enum class Type : uint8_t {
    Void,
    Bool,
    I32,
    F64,
    Str,      // native string (representation decided in)
    Dynamic,  // boundary-only boxed value; using it is an explicit opt-in
    // A raw 64-bit word that is NOT a Value and not a GC root: a shape word
    // (`load.shape`, `shape.cell`). Only `ptr.eq` consumes one. Shapes are
    // immortal, so a ptr held across a collection stays valid; it must never
    // be boxed, stored or passed to a call.
    Ptr,
};
const char* typeName(Type t);

using ValueId = uint32_t;
inline constexpr ValueId kNoValue = UINT32_MAX;

using BlockId = uint32_t;
inline constexpr BlockId kNoBlock = UINT32_MAX;

struct BlockParam {
    ValueId id = kNoValue;
    Type type = Type::Void;
};

struct BlockTarget {
    BlockId block = kNoBlock;
    std::vector<ValueId> args;
};

// A field added to Instruction, Block, Param, Function or Module is a field the
// on-disk code cache must carry: add it to il/serialize.cpp and bump its
// kFormatVersion (the layout guards there fail the build until the first is done).
struct Instruction {
    Op op;
    Type type = Type::Void;          // result type (Void: no result)
    ValueId result = kNoValue;
    std::vector<ValueId> operands;
    double immF64 = 0;               // ConstF64
    int32_t immI32 = 0;              // ConstI32 / CreateArray length / CreateFunction arity
    uint32_t calleeIndex = 0;        // Call/CreateFunction: index into Module::functions
    // The OPERAND's type, for the two ops whose behaviour turns on it and whose
    // own `type` is the result: Box (what is being boxed) and ToInt32 (whether
    // step 1's ToNumber is a machine conversion or a call that can throw).
    // Not printed for ToInt32 — `to.int32 %n` names one operation either way,
    // and the operand's own definition already says what type it is.
    Type boxType = Type::Void;
    uint32_t keyIndex = 0;           // PropGet/PropSet: key constant index
    uint32_t icIndex = 0;            // PropGet/PropSet: IC site index
    // A SLOW SITE (il/feedback_site.h numberSlowSites): the index of the counter a build that
    // can be harvested bumps each time one of this instruction's inline fast
    // paths falls to its generic code, or kNoSlowSite. Handed out once, at the
    // start of the final passes, so the build a harvest came from and the one
    // that reads it number the same instructions alike; copies share it.
    static constexpr uint32_t kNoSlowSite = UINT32_MAX;
    uint32_t slowSite = kNoSlowSite;
    // Construct: the module function a `new` whose callee is written as the
    // name of a function declaration lowered before it enters
    // (lower_object.cpp), or kNoDirectTarget. A `new` whose plain-name callee
    // was not yet lowered carries the name in `keyIndex` and kConstructNamed
    // in `immI32` instead.
    static constexpr uint32_t kNoDirectTarget = 0xFFFFFFFFu;
    uint32_t directTarget = kNoDirectTarget;
    static constexpr int32_t kConstructNamed = 1;
    uint32_t envDepth = 0;           // EnvGet/EnvSet: parent hops
    uint32_t envIndex = 0;           // EnvGet/EnvSet: slot within that environment
    // EnvGet: slot is an initialized immutable binding. EnvGetTdz: once
    // initialized, the slot is never written again (lower_scope.cpp).
    bool envImmutable = false;
    // A direct `call` to a CLOSURE, and the number of parent links from the
    // record in operand 0 to the one the closure captured.
    //
    // A direct call normally cannot reach a closure at all: `__env` arrives
    // through the dynamic calling convention, and a call site that named the
    // function without supplying one would enter it with garbage. This says the
    // site supplies it — not by loading the closure value, which would put the
    // whole guarded environment read back on the fast path, but by handing over
    // the CALLER's own record and a hop count the scope plan established
    // (lower_scope.cpp, `planStableFunctionSlots`). Operand 0 is that record,
    // and it occupies the callee's `__env` parameter.
    //
    // `kNoEnvHops` — the default — means the site supplies no environment, and
    // the verifier then holds the original rule: a direct call names a function
    // that needs none.
    static constexpr uint32_t kNoEnvHops = 0xFFFFFFFFu;
    uint32_t callEnvHops = kNoEnvHops;
    // A direct `call` in a speculated function S that the speculation step
    // made from a generic call of S's G (or found there, lowered): the id of
    // the `resume.point` it placed in G immediately AFTER that generic call,
    // whose state is what G needs from there on, the call's result among it
    // when it is used. It is what lets the inliner (opt/inline) replace the
    // call with the callee's body: a guard of the inlined body that fails
    // finishes the call generically (`resume.call`) and leaves for this point.
    // Meaningful only to the inliner, which clears it on every instruction
    // when it is done; kNoResumeAfter where no point could be placed (a
    // protected block), and on every other instruction.
    static constexpr uint32_t kNoResumeAfter = 0xFFFFFFFFu;
    uint32_t resumeAfter = kNoResumeAfter;

    // The inlined call this instruction's code came from (opt/inline): a
    // 1-based index into its function's `inlineScopes`, 0 for the function's
    // own code. What an Error.stack frame for a position here names.
    uint32_t inlineScope = 0;

    Span span;

    BlockTarget target;              // Jump target / Branch then-target
    BlockTarget elseTarget;          // Branch else-target
};

// Whether this non-terminator can throw, so that the backend emits an
// exception test after it. Defined in effects.cpp as a reader of the effect
// table (il/effects.h), which is what every pass asks instead.
bool canThrow(const Instruction& inst);

// Whether running this instruction can move a heap object: it allocates, or it
// reaches user code that might. STRICTLY WIDER than `canThrow` — an allocation
// that fails is fatal rather than catchable, so `create.object` throws nothing
// and collects anyway — and the two are not interchangeable at a use site.
//
// It exists for the backend's receiver proof (llvm_recv_proof.h), which holds a
// pointer DERIVED from a heap object across several instructions. A derived
// pointer is not a GC root: nothing forwards it, so it is valid exactly as far
// as the next collection, and this predicate is where that distance is
// measured.
//
// The default is TRUE. Only ops enumerated as neither allocating nor reaching
// user code answer false, so an op added later is a missed optimisation rather
// than a dangling pointer.
bool canCollect(const Instruction& inst);

// The WRITE MODE `prop.set` and `elem.set` carry in `immI32`. An assignment is
// sloppy (0) or strict (1), the refusal rule 13.15.2 applies; a DEFINITION (2)
// is CreateDataPropertyOrThrow — an object literal's properties, a class's
// fields — which never walks the prototype chain, so no inherited setter runs
// and no inherited read-only property refuses. The two must never share a
// path that could answer one with the other: every consumer that keys a fast
// path off a `prop.set` asks `isDefineWrite` first (the backend lowers a define
// to its own ops, il_to_brass_ast.cpp).
inline constexpr int32_t kWriteSloppy = 0;
inline constexpr int32_t kWriteStrict = 1;
inline constexpr int32_t kWriteDefine = 2;
// An ARRAY LITERAL's element (`[a, b]`'s `0` and `1`): CreateDataProperty on
// the fresh array, which is an assignment's `prop.set` in every path the
// backend gives one — the literal's index stores are the dense-array fast
// paths' own — except its miss, which defines rather than sets (bronze_prop_
// define). The fresh array's slots are holes until written, and a [[Set]]
// into a hole asks the chain, so a setter on `Object.prototype[1]` would
// otherwise run for `[x, y]`. Not a define (isDefineWrite) for that reason:
// the fast paths stay.
inline constexpr int32_t kWriteLiteralElement = 3;

// An `elem.set` carries element flags above its mode (il_pin.h
// kElemFlagArrayReceiver, kElemFlagProvenIntegralIndex).
inline constexpr int32_t kWriteModeMask = 0xFF;
inline constexpr int32_t kElemSetFlagBits =
    kWriteModeMask | kElemFlagArrayReceiver | kElemFlagProvenIntegralIndex | kElemFlagWordViewReceiver;
inline int32_t writeMode(const Instruction& inst) { return inst.immI32 & kWriteModeMask; }

inline bool isDefineWrite(const Instruction& inst) {
    return (inst.op == Op::PropSet || inst.op == Op::ElemSet) && writeMode(inst) == kWriteDefine;
}

struct Block {
    BlockId id = 0;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
    // A block lowering expects not to be taken (lower_pattern.cpp). The
    // backend lays it out cold, after the function's main line; it is a
    // layout hint and never changes what the block does.
    bool cold = false;
    // Where control goes if something inside this block throws: the innermost
    // enclosing handler, or `kNoBlock` for "leave the function". It sits on
    // the block rather than on each call: the backend makes each call in the
    // block an invoke whose unwind edge is this handler, and the block is
    // never split in the IL, so no join here grows a parameter.
    //
    // A handler block therefore takes NO IL parameters: it is entered from an
    // arbitrary point in the protected region, and nothing here knows what a
    // binding held there, which is what an environment record makes possible.
    // The thrown value reaches it through `exc.take`.
    BlockId handler = kNoBlock;
};

struct Param {
    std::string name;
    Type type;
    // This slot is `F64` because a `--pins` SIGNATURE entry said so, and no
    // proof agrees (src/types/pins.h, stage B1). It is the licence for the
    // barrier the boxed wrapper and the enumerated call sites emit, and it is
    // deliberately NOT "the type is F64": a parameter typed by
    // `applyProvenSignature` or by the closure parameter proof has a proof
    // behind it, and re-checking a proof would tax the programs that need no
    // barrier at all. The pin index names the manifest line for the message.
    bool pinned = false;
    // The key-constant index of the manifest line this pin came from, valid
    // only when `pinned`.
    uint32_t pinKeyIndex = 0;
};

struct Function {
    std::string name;
    // Synthetic leading parameters come first, in this order: [__env if
    // needsEnv] [__this if needsThis] [__arguments if needsArguments] source
    // parameters... __env can only come from the closure, so a needsEnv
    // function is never a direct-call target; __this is supplied by every
    // caller, including direct ones, so needsThis carries no such restriction.
    std::vector<Param> params;
    Type returnType = Type::Void;
    // `returnType` is `F64` because a `--pins` `return <owner>: number` entry
    // said so, with no proof agreeing — the return half of `Param::pinned`,
    // and the licence for the barrier `lowerReturnStmt` puts on a returned
    // value it cannot type. `pinKeyIndex` names the manifest line.
    bool returnPinned = false;
    uint32_t returnPinKeyIndex = 0;
    bool isExported = false;
    // A DECLARATION rather than a definition: no blocks, and the name is a
    // symbol the backend resolves — a runtime helper the lowering calls by
    // name (the native argument helpers), or a native import thunk
    // (`__bronze_native_<i>`, see Module::nativeImports). Skipped by the
    // verifier's body checks and printed as an `extern` line.
    bool isExternal = false;
    // The module's entry point: the function the backend names `main`, which
    // a host enters from C++ (runtime/exception.h, rtRunModuleEntry, is where
    // an uncaught throw out of it is reported). A flag rather than a name
    // comparison in the backend, because "which function is `main`" is a fact
    // lowering knows and codegen should not have to spell.
    bool isEntryPoint = false;
    bool needsEnv = false;
    bool needsThis = false;
    // `arguments`: every argument the caller really passed, as one array. Like
    // the rest array it can only be built where the true argument count is
    // visible, which is the call WRAPPER — so a function that needs one is
    // never a direct-call target, the same restriction `needsEnv` carries and
    // for a related reason.
    bool needsArguments = false;
    // The body only reads `arguments.length` and `arguments[i]`
    // (ast::readsArgumentsAsList): the wrapper passes a plain Array.
    bool argumentsIsList = false;
    // With `argumentsIsList`, and every mention in the body itself
    // (ast::readsArgumentsInBodyOnly): a call that passes no more arguments
    // than there are source parameters hands `__arguments` the COUNT, a
    // Number, and builds no Array. The body tests which it holds: the length
    // is the count, and an element is read off an Array of the values the
    // parameters arrived with (lower_object.cpp `lazyArgumentsList`).
    bool argumentsIsCount = false;
    bool isStrict = false;
    bool isGenerator = false;
    // The BODY of a generator, async function or async generator: a coroutine
    // (src/lower/lower_generator.cpp). The function of that name is a stub
    // whose `coro.start` creates a brass coroutine frame of this body with the
    // stub's own parameters as arguments, and the body suspends at
    // `coro.suspend`. So it is never called: the backend gives it the frame as
    // a leading parameter and no call wrapper, and brass's coroutine lowering
    // keeps its values across suspensions.
    static constexpr int8_t kNotCoroutine = -1;
    int8_t coroKind = kNotCoroutine;  // BRONZE_ABI_CORO_* for a body
    bool isCoroutineBody() const { return coroKind != kNotCoroutine; }
    // The BRONZE_ABI_FN_FLAG_* byte the created function object carries: what
    // its syntax decided about [[Construct]] and about the `prototype`
    // property (src/abi/bronze_abi.h says why neither is derivable here).
    // `isGenerator` above is a different question — it says the body starts
    // parked and `yield`s — and the two are set from the same AST node without
    // either being read off the other.
    uint32_t fnFlags = BRONZE_ABI_FN_FLAGS_ORDINARY;
    // `...rest`: the LAST source parameter, and the one no caller supplies a
    // value for. It arrives as an array built from whatever arguments were left
    // over — by the call wrapper on the uniform path, by the call site on a
    // direct one.
    bool hasRestParam = false;
    // The parameters before the first one with a default. NOT a minimum a call
    // has to meet — the language has no arity error, and a call short of this
    // is filled with `undefined` at the call site exactly as one short of the
    // fixed parameter count is, with the callee's prologue deciding what that
    // means.
    //
    // It is also, exactly, ECMA-262 15.1.5 ExpectedArgumentCount — so it is
    // what 10.2.10 SetFunctionLength makes the function's `length` property,
    // and the backend passes it through as such. Two facts, one count, and
    // deliberately not two fields: a program that could see them disagree would
    // be seeing a bug.
    uint32_t requiredArgs = 0;
    // 10.2.9 SetFunctionName's answer for this function, as an index into
    // `Module::keyConstants`. It is NOT `name` above: that is the IL's own
    // identifier, synthesized as `__anon_fn_N` for a function the source did
    // not name, while this is the string the language says `f.name` is — "" for
    // a genuinely anonymous function, the binding's name under NamedEvaluation
    // (8.6.2), and "get x" / "set x" for an accessor.
    //
    // `BRONZE_ABI_FN_NAME_NONE` means the name was never recorded, which a
    // program can only reach for a function whose name is computed at runtime
    // (`class C { [k]() {} }`); reading `.name` off one is then a diagnosed
    // refusal rather than a wrong answer.
    uint32_t nameKeyIndex = 0xFFFFFFFFu;
    // The bytes of source this function was written from, as a half-open range
    // into `Module::sourceTexts[sourceFile]`. It is what 20.2.3.5
    // Function.prototype.toString returns, which the spec makes the SOURCE
    // TEXT verbatim and not a reconstruction — so nothing downstream may
    // re-render it, and the range has to be exact: `sourceEnd` is the end of
    // the function's last token, never the start of the next one.
    //
    // For a class it is the whole `class C { ... }`, because the constructor
    // IS the class value and `C.toString()` is the class's text (15.7.14 makes
    // the class's source text the constructor's [[SourceText]]).
    //
    // `sourceEnd == 0` means no text was recorded — a function bronze
    // synthesized, or a build that passed `--no-fn-source`.
    uint16_t sourceFile = 0;
    uint32_t sourceBegin = 0;
    uint32_t sourceEnd = 0;
    std::string displayName;
    uint32_t descFlags = 0;

    // Index of the first source-level parameter.
    size_t firstSourceParam() const {
        return static_cast<size_t>(needsEnv) + static_cast<size_t>(needsThis) +
               static_cast<size_t>(needsArguments);
    }
    // How many arguments a CALLER passes: every source parameter but the
    // rest one, which is not a value the convention carries.
    size_t callerParamCount() const {
        return params.size() - firstSourceParam() - static_cast<size_t>(hasRestParam);
    }
    // The arity a short call is PADDED to with `undefined`. Zero means "do not
    // pad", which is what a function owning an `arguments` object needs: the
    // object is built from the argument count the wrapper sees, and padding
    // would make `f(1)` indistinguishable from `f(1, undefined)` where the
    // language says `arguments.length` is 1 and 2. Its wrapper reads argv
    // through `bronze_arg_at` instead.
    uint32_t adaptArity() const {
        return needsArguments ? 0u : static_cast<uint32_t>(callerParamCount());
    }
    // A SPECULATED function S: the index in Module::functions of the generic
    // function G it was cloned from, which every `leave` in S resumes. G
    // itself, and every function lowering makes, is kNotSpeculated. A
    // speculated function holds no `resume.point`; a generic one no `leave`.
    static constexpr uint32_t kNotSpeculated = UINT32_MAX;
    uint32_t speculatedFrom = kNotSpeculated;
    bool isSpeculated() const { return speculatedFrom != kNotSpeculated; }
    // An OPTIMISTIC speculated function S+ (opt/speculate optimistic.h): the
    // index of the conservative speculated function S of the same G that its
    // retirement falls back to. S+ is S with assumptions that pay only when a
    // miss leaves (a block the harvested run never entered leaves instead of
    // rejoining); it is entered first, and its optimistic exits (resume
    // points of kind kResumeOptimistic) retire it to S, not to G. kNoFallback
    // for every other function.
    static constexpr uint32_t kNoFallback = UINT32_MAX;
    uint32_t fallback = kNoFallback;
    bool isOptimistic() const { return fallback != kNoFallback; }
    // The slow-site index of the function's ENTRY COUNT (il/feedback_site.h
    // numberSlowSites): a build made to be harvested adds one to its word at
    // every entry. A speculated copy keeps its G's. Instruction::kNoSlowSite
    // for a function given none (an external one, a body with no blocks).
    uint32_t entrySlowSite = UINT32_MAX;
    // What the harvest a build spent says of this function's run, set by the
    // speculation step (opt/speculate driver.cpp, Evidence::heat) and kept by
    // every copy made of it: Unknown without a harvest (or for a function the
    // harvested build did not count), Never for one it never entered,
    // Entered for one it entered fewer than kMinEntries times and did not
    // find hot (Evidence::isHot), Hot for the rest. `harvestEntries` is the
    // count. Hot is not "speculated": a function entered a million times
    // that the sampler never caught is Hot here and may have no S.
    enum class Heat : uint8_t { Unknown, Never, Entered, Hot };
    Heat heat = Heat::Unknown;
    uint64_t harvestEntries = 0;
    // The calls the inliner replaced by their callee's body: scope k + 1 is
    // the body of module function `callee` (whose frame a stack trace shows)
    // called at `call`, in scope `parent` (0: this function's own code).
    struct InlineScope {
        uint32_t callee = 0;
        Span call;
        uint32_t parent = 0;
    };
    std::vector<InlineScope> inlineScopes;
    std::vector<Block> blocks;
    uint32_t valueCount = 0;  // number of ValueIds in use (params first)
};

// Whether compiled code may enter a function object made from `fn` without
// the runtime (bronze_abi.h, BRONZE_ABI_FN_FLAG_DIRECT_CALL): its body never
// reads the new.target scope a runtime call would have pushed — no
// `get.new_target` and no `super()` — and it is neither a generator nor an
// async function, whose calls start a coroutine; and, for a speculated
// function, its generic function enters directly too, since a guard that
// leaves finishes the call there (and an optimistic copy may have cut every
// reader out of its own body). The one predicate the flag is set from;
// defined in effects.cpp.
struct Module;
bool functionEntersDirectly(const Module& module, const Function& fn);

struct Module {
    std::string name;
    std::vector<std::string> keyConstants;
    // How many inline-cache sites lowering handed out across the whole module.
    // The backend emits exactly this many entries as a global array in the
    // object file, so it is the size of a real allocation, not a hint: the
    // verifier checks every icIndex against it.
    uint32_t icSiteCount = 0;
    // How many TAGGED TEMPLATE sites lowering handed out. Sized the same way
    // `icSiteCount` is and for the same reason: the backend emits exactly this
    // many Value cells as a global array, and each site's cell holds the one
    // template object 13.2.8.4 GetTemplateObject requires that site to hand its
    // tag function on every call.
    uint32_t templateSiteCount = 0;
    // How many SLOW SITES (Instruction::slowSite) the final passes handed
    // out: the backend emits this many zeroed u64 counters right before the
    // IC table, and the harvest reads them back (docs/type-feedback.md,
    // "Slow paths").
    uint32_t slowSiteCount = 0;
    // The PRETENURED allocation sites, by slow site (ascending): sites whose
    // objects a harvest saw nearly all tenured, which the backend allocates
    // straight into the old generation (docs/type-feedback.md,
    // "Pretenuring"). An object literal, a construct, and the `create.this`
    // a speculated construct makes (which carries the construct's site).
    std::vector<uint32_t> pretenureSites;
    // Whether `global.get "Math"` / `global.get "Number"` name the builtins:
    // lockdown freezes them and pins both bindings (docs/internals.md,
    // "Realms and lockdown"), so only a host that claims the name
    // (`--host-globals`) can make them anything else. A program's own
    // `Math` is a lexical binding and never lowers to global.get. Set by
    // lowering; the speculation step computes `Math.sin(x)` with no read of
    // the property and no identity test when it holds.
    bool mathPinned = false;
    bool numberPinned = false;
    // THE CENSUS SITE TABLE, the runtime channel a type-feedback build
    // (`--feedback-out`, lower/lower_feedback.cpp) registers its sites
    // through (abi/bronze_feedback_site.h): handed to the runtime at module
    // init, so a site the run never reaches is still known.
    struct CensusSiteEntry {
        uint32_t keyIndex = 0;  // into `keyConstants`
        uint32_t info = 0;      // BRONZE_FEEDBACK_INFO kind | ordinal
    };
    std::vector<CensusSiteEntry> censusSites;
    // Where the instrumented run writes its profile. Empty unless
    // `--feedback-out`.
    std::string censusOutPath;
    // THE INLINE-CACHE HARVEST (docs/type-feedback.md, "Harvest"). The image
    // carries an identity record its module init hands the runtime with the
    // IC table: `icLayoutDigest`, a hash of the source texts and of what every
    // IC index names (il/feedback_site.h icLayoutDigest), as the final passes
    // left them; and `harvestOutPath`
    // (`--harvest-out`), where a standalone program writes its harvest.
    uint64_t icLayoutDigest = 0;
    std::string harvestOutPath;
    // THE NATIVE IMPORT TABLE (`--native-manifest`; bronze_abi.h's loadable-
    // module section): one entry per native the program calls directly, in
    // the order lowering first met them. Entry `i` is reached through the
    // external IL function `__bronze_native_<i>`, whose parameter and return
    // types are the native's C signature in IL terms (a typed array is two
    // parameters, pointer then length; a handle or pointer is `Dynamic`, the
    // 64-bit word). The backend emits the table as `<entry>_native_imports`
    // and one thunk per entry that loads slot `i` and calls through it; a
    // `class <path>` entry's thunk takes nothing and returns the slot itself,
    // which is the class tag the argument helpers compare against.
    struct NativeImport {
        std::string name;       // "<kind> <path>", or "class <path>"
        std::string signature;  // canonical text; empty for a class slot
        uint32_t functionIndex = 0;  // the `__bronze_native_<i>` declaration
        // A native answering `T[]`: the ElementKind of T (the ABI's
        // numbering, abi/bronze_native_type.h), and the thunk passes a
        // descriptor slot as the C function's trailing argument and wraps
        // it after the call. UINT32_MAX for every other return type.
        uint32_t bufferReturnKind = UINT32_MAX;
    };
    std::vector<NativeImport> nativeImports;
    // THE LAYOUT CELLS `shape.cell <n>` reads: one shape word per entry, which
    // the module's loader fills once, before any code runs, by resolving
    // `key` — the shape's name as the harvest spells it (opt/spec/profile.h,
    // the `shapes` rows) — against the shapes the runtime has made. The key
    // is all a loader needs: a shape it cannot resolve leaves the cell
    // holding a word no object carries, so every test against it fails and
    // the speculated code leaves. Keys are unique within the table.
    struct ShapeCell {
        std::string key;
    };
    std::vector<ShapeCell> shapeCells;
    // THE NUMBER-SLOT KEYS (opt/speculate/number_slots.h): key constant
    // indices the module registers at init as eligible for a double slot
    // (bronze_register_slot_repr, runtime/slot_repr.h), before any code
    // makes an object. The speculation step sets them from a harvest; the
    // shape cells it makes spell those keys' double slots `%k`.
    std::vector<uint32_t> numberSlotKeys;
    // THE OBJECT LITERALS `object.literal <lit>` makes: entry `lit` is the
    // literal's keys (indices into `keyConstants`, distinct named keys, in
    // source order) and the inline-slot capacity its objects are made with
    // (0: the runtime's default; lower_object.cpp literalInlineSlots). The
    // backend gives each a per-thread word holding the shape the runtime
    // built for it the first time (il2mir il_literal_lowering.cpp).
    struct ObjectLiteral {
        std::vector<uint32_t> keys;
        uint32_t inlineSlots = 0;
    };
    std::vector<ObjectLiteral> objectLiterals;
    // THE TRANSITION CELLS `proto.cell <c>` tests: each speaks for one
    // property ADD, of key `keyIndex` to an object whose shape is the one
    // shape cell `fromCell` holds, giving the shape shape cell `toCell` holds
    // with the key at inline slot `slot`. The runtime stamps a cell with the
    // prototype epoch when asked (`proto.publish`), after checking that the
    // add is a plain data property add the fast path can make: `from` a
    // plain shape no object is a prototype through, `to` its child for the
    // key, boxed, writable, enumerable, configurable, inline at `slot`, and
    // no setter, read-only property or proxy for the key on the chain above
    // (runtime/transition_cells.cpp). A bump of the epoch retires every
    // stamp at once.
    struct ProtoCell {
        uint32_t fromCell = 0;
        uint32_t keyIndex = 0;
        uint32_t toCell = 0;
        int32_t slot = 0;
        bool operator==(const ProtoCell&) const = default;
    };
    std::vector<ProtoCell> protoCells;
    // MEMBER CELLS (runtime/accessor_cell.cpp): what lets a speculated
    // method call, getter read or setter write whose member lives above the
    // receiver be a shape test and a direct call. A cell is two words in the
    // module's per-thread data, (shape, epoch); `member.cell c` reads the
    // shape word and `member.publish` asks the runtime to fill it. The
    // cells of one site form a GROUP (`group` its first cell, `groupSize`
    // its cells, in order), each with its own function; a shape the group
    // holds is put in no second cell of it. An ABSENT cell holds a shape
    // whose chain has the key nowhere, so a read of it is `undefined`; it
    // names no function (`callee` is kNoMemberCallee). A DATA cell holds a
    // shape whose chain finds the key as a data property `depth` links up
    // at `slot` of that holder, so a read of it is the walk and the slot's
    // load; its `callee` is memberDataPlace(depth, slot).
    enum class MemberKind : uint8_t { Method, Getter, Setter, Absent, Data };
    static constexpr uint32_t kNoMemberCallee = UINT32_MAX;
    static constexpr uint32_t kMemberDataSlotBits = BRONZE_ABI_MEMBER_DATA_SLOT_BITS;
    static constexpr uint32_t memberDataPlace(uint32_t depth, uint32_t slot) {
        return (depth << kMemberDataSlotBits) | slot;
    }
    struct MemberCell {
        MemberKind kind = MemberKind::Method;
        uint32_t keyIndex = 0;
        uint32_t callee = 0;  // module function whose code the cell's function runs
        uint32_t group = 0;
        uint32_t groupSize = 1;
        bool operator==(const MemberCell&) const = default;
    };
    std::vector<MemberCell> memberCells;
    // A deque, not a vector: lowering a function body can append nested
    // closures, and the body being lowered is itself an element. Only a
    // reference-stable container lets a recursive call read its own
    // (still-being-inferred) signature without dangling on a reallocation.
    std::deque<Function> functions;
    // Every source file of the program, indexed by `Function::sourceFile`, so
    // that a function's `sourceBegin`/`sourceEnd` name real bytes. ONE COPY of
    // each file rather than a string per function: three.js's 28 files are
    // 1.6 MB together, and the ~3000 functions in them overlap almost
    // completely, so per-function strings would be that figure many times over.
    //
    // Empty when the build asked for no function source, which is the whole of
    // what `--no-fn-source` does — the ranges above stay, and address nothing.
    std::vector<std::string> sourceTexts;
    std::vector<std::string> sourceFiles;
    // The line index of each file, same order, built with the texts and kept
    // when they are dropped: every (line, column) the backend records — the
    // pc tables behind Error.stack and each descriptor's definition
    // position — is a span resolved against this, and a build without
    // function source still has to put its frames on the right line.
    std::vector<LineTable> lineTables;

    // The inline-cache sites that belong to METHOD CALLS, in ascending order:
    // the `icIndex` of every MethodCall / MethodCallSpread, each once. A
    // method site's words mean something different from a property site's
    // (bronze_abi.h, "the METHOD-CALL site"), and its env words can hold heap
    // Values, so the backend hands exactly this list to
    // `bronze_register_method_ic_cells` at module init. Derived from the
    // instructions rather than kept as a field, so it cannot disagree with
    // them; the verifier is what guarantees no index is shared between a
    // method call and a property access.
    std::vector<uint32_t> methodIcSites() const {
        std::vector<uint32_t> sites;
        for (const auto& fn : functions) {
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == Op::MethodCall || inst.op == Op::MethodCallSpread) {
                        sites.push_back(inst.icIndex);
                    }
                }
            }
        }
        std::sort(sites.begin(), sites.end());
        sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
        return sites;
    }
};

}  // namespace bronze::il
